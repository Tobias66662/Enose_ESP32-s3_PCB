#pragma once

#include <cstring>

struct DeviceProfile
{
    const char *mqtt_client_id;
    const char *mqtt_publish_topic;
    const char *mqtt_subscribe_topic;
};

/* Device selector.
 * Set this to one of "1A", "1B", "2A", "2B", "3A", or "3B". (currently limited to 3 but we can easily add more)
 * Example: "1A" compiles telescent_device_1A, publishes to telescent/device1A, and subscribes to telescent/device1B.
 * The device that will communicate with 1A should have Device_ID set to "1B", 2A should communicate with 2B, and 3A should communicate with 3B and so on...
 */
inline constexpr const char *Device_ID = "1A";

inline constexpr DeviceProfile kDevice1A{
    "telescent_device_1A",
    "telescent/device1A",
    "telescent/device1B"};

inline constexpr DeviceProfile kDevice1B{
    "telescent_device_1B",
    "telescent/device1B",
    "telescent/device1A"};

inline constexpr DeviceProfile kDevice2A{
    "telescent_device_2A",
    "telescent/device2A",
    "telescent/device2B"};

inline constexpr DeviceProfile kDevice2B{
    "telescent_device_2B",
    "telescent/device2B",
    "telescent/device2A"};

inline constexpr DeviceProfile kDevice3A{
    "telescent_device_3A",
    "telescent/device3A",
    "telescent/device3B"};

inline constexpr DeviceProfile kDevice3B{
    "telescent_device_3B",
    "telescent/device3B",
    "telescent/device3A"};

inline const DeviceProfile &selected_device_profile()
{
    if (std::strcmp(Device_ID, "1B") == 0)
    {
        return kDevice1B;
    }

    if (std::strcmp(Device_ID, "2A") == 0)
    {
        return kDevice2A;
    }

    if (std::strcmp(Device_ID, "2B") == 0)
    {
        return kDevice2B;
    }

    if (std::strcmp(Device_ID, "3A") == 0)
    {
        return kDevice3A;
    }

    if (std::strcmp(Device_ID, "3B") == 0)
    {
        return kDevice3B;
    }

    return kDevice1A;
}