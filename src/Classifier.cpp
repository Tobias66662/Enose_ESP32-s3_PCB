#include "Classifier.h"

#include "edge-impulse-sdk/classifier/ei_classifier_types.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "edge-impulse-sdk/dsp/returntypes.h"


constexpr char kTag[] = "Classifier";
constexpr uint8_t kRequiredConsecutivePredictions = 10;
constexpr float kRequiredAverageConfidence = 0.80f;

constexpr size_t kFeaturesPerSample = 5;
constexpr size_t kWindowSize = 10;
constexpr size_t kInputSize = kFeaturesPerSample * kWindowSize;

float featureBuffer[kInputSize] = {};
size_t bufferCount = 0;
bool bufferFilled = false;

char lastPredictionLabel[32] = "";
uint8_t consecutivePredictionCount = 0;
float recentPredictionScores[kRequiredConsecutivePredictions] = {0.0f};
uint8_t recentPredictionScoreCount = 0;
uint8_t recentPredictionScoreIndex = 0;
float recentPredictionScoreSum = 0.0f;

QueueHandle_t classifier_label_queue = nullptr;

void classifierTask(void *parameter);

void classifier_init()
{
  classifier_label_queue = xQueueCreate(5, sizeof(classifier_label_t));

  if (classifier_label_queue == nullptr)
  {
    ESP_LOGE(kTag, "Failed to create classifier_label_queue");
  }

  xTaskCreate(
    classifierTask,
    "ei_classifier",
    12288,          // Bytes allocated in memory to the task
    nullptr,
    4,              // task priority
    nullptr);
}

namespace {

void addToBuffer(const float *newData)
{
  memmove(
    featureBuffer,
    featureBuffer + kFeaturesPerSample,
    sizeof(float) * kFeaturesPerSample * (kWindowSize - 1));

  memcpy(
    featureBuffer + kFeaturesPerSample * (kWindowSize - 1),
    newData,
    sizeof(float) * kFeaturesPerSample);

  if (!bufferFilled)
  {
    bufferCount++;
    if (bufferCount >= kWindowSize)
    {
      bufferFilled = true;
    }
  }
}

void resetConsensusState()
{
  lastPredictionLabel[0] = '\0';
  consecutivePredictionCount = 0;
  recentPredictionScoreCount = 0;
  recentPredictionScoreIndex = 0;
  recentPredictionScoreSum = 0.0f;
}

bool hasConsensusLabel(const char *predictionLabel, float score)
{
  if (predictionLabel == nullptr)
  {
    resetConsensusState();
    return false;
  }

  if (strcmp(lastPredictionLabel, predictionLabel) == 0)
  {
    if (consecutivePredictionCount < 255)
    {
      consecutivePredictionCount++;
    }

    if (recentPredictionScoreCount < kRequiredConsecutivePredictions)
    {
      recentPredictionScores[recentPredictionScoreCount] = score;
      recentPredictionScoreCount++;
      recentPredictionScoreSum += score;
    }
    else
    {
      recentPredictionScoreSum -= recentPredictionScores[recentPredictionScoreIndex];
      recentPredictionScores[recentPredictionScoreIndex] = score;
      recentPredictionScoreSum += score;
      recentPredictionScoreIndex = (recentPredictionScoreIndex + 1) % kRequiredConsecutivePredictions;
    }
  }
  else
  {
    strncpy(lastPredictionLabel, predictionLabel, sizeof(lastPredictionLabel) - 1);
    lastPredictionLabel[sizeof(lastPredictionLabel) - 1] = '\0';
    consecutivePredictionCount = 1;

    recentPredictionScoreCount = 1;
    recentPredictionScoreIndex = 0;
    recentPredictionScoreSum = score;
    recentPredictionScores[0] = score;
  }

  if (consecutivePredictionCount < kRequiredConsecutivePredictions)
  {
    return false;
  }

  const float averageScore = recentPredictionScoreSum / static_cast<float>(recentPredictionScoreCount);
  return averageScore >= kRequiredAverageConfidence;
}

const char *runInference(float *confidenceOut)
{
  if (!bufferFilled)
  {
    return nullptr;
  }

  ei::signal_t signal;
  const int err = ei::numpy::signal_from_buffer(featureBuffer, kInputSize, &signal);
  if (err != 0)
  {
    ESP_LOGE(kTag, "Signal error: %d", err);
    return nullptr;
  }

  ei_impulse_result_t result = {};
  const EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);
  if (res != EI_IMPULSE_OK)
  {
    ESP_LOGE(kTag, "Inference failed: %d", res);
    return nullptr;
  }

  float maxValue = result.classification[0].value;
  const char *bestLabel = result.classification[0].label;

  for (size_t i = 1; i < EI_CLASSIFIER_LABEL_COUNT; i++)
  {
    if (result.classification[i].value > maxValue)
    {
      maxValue = result.classification[i].value;
      bestLabel = result.classification[i].label;
    }
  }

  ESP_LOGI(kTag, "Prediction: %s (%.3f)", bestLabel, maxValue);

  if (confidenceOut != nullptr)
  {
    *confidenceOut = maxValue;
  }

  return bestLabel;
}

const char *applyConsensusLabel(const char *predictionLabel, float score)
{
  if (predictionLabel == nullptr)
  {
    resetConsensusState();
    return "empty";
  }

  if (!hasConsensusLabel(predictionLabel, score))
  {
    const float averageScore = recentPredictionScoreCount > 0
      ? recentPredictionScoreSum / static_cast<float>(recentPredictionScoreCount)
      : 0.0f;

    ESP_LOGI(
      kTag,
      "No consensus yet -> output empty (count=%u/%u, avg_last_%u=%.3f, threshold=%.3f)",
      consecutivePredictionCount,
      kRequiredConsecutivePredictions,
      kRequiredConsecutivePredictions,
      averageScore,
      kRequiredAverageConfidence);

    return "empty";
  }

  const float averageScore = recentPredictionScoreSum / static_cast<float>(recentPredictionScoreCount);
  ESP_LOGI(
    kTag,
    "Consensus label: %s avg=%.3f threshold=%.3f",
    predictionLabel,
    averageScore,
    kRequiredAverageConfidence);

  return predictionLabel;
}

}  // end namespace

void classifierTask(void *parameter)
{
  (void)parameter;

  run_classifier_init();

  sensor_readings_t reading = {};
  TickType_t last_label_send_time = xTaskGetTickCount() - pdMS_TO_TICKS(30000);

  while (true)
  {
    if (xQueueReceive(sensor_readings_queue, &reading, portMAX_DELAY) == pdTRUE) // Recieve one reading
    {
      const float features[kFeaturesPerSample] = { // convert it to features
        reading.sen66_temperature,
        reading.sen66_humidity,
        reading.voc_index,
        static_cast<float>(reading.co2),
        reading.hcho,
      };

      addToBuffer(features); // add it to the sliding buffer

      if (bufferFilled) // only do stuff if the buffer is full
      {
        float confidence = 0.0f;
        const char *bestLabel = runInference(&confidence); //run classifier on the newest buffer contents
        const char *resolvedLabel = applyConsensusLabel(bestLabel, confidence); // apply consensus filtering

        ESP_LOGI(kTag, "Resolved label: %s", resolvedLabel);

        if (strcmp(resolvedLabel, "empty") != 0) // don't sent to queue if label contains "empty"
        {
          TickType_t now = xTaskGetTickCount();

          if ((now - last_label_send_time) >= pdMS_TO_TICKS(30000)) // Only add new lable to queue if 30 seconds have pased since last label was added
          {
            // store the string pointed to by *resolvedLabel in a custom type that can be added to the queue
            ESP_LOGI(kTag, "%s label sent to classifier_label_queue", resolvedLabel);
            classifier_label_t resolved_label = {};
            strncpy(resolved_label.label, resolvedLabel, sizeof(resolved_label.label) - 1);
            resolved_label.label[sizeof(resolved_label.label) - 1] = '\0';

            if (xQueueSend(classifier_label_queue, &resolved_label, pdMS_TO_TICKS(100)) == pdFALSE) // Send label to Queue
            {
              ESP_LOGW("Classifier", "Classifier label queue full");
            }

            last_label_send_time = now;
          }
        }

      }

    }

  }
}
