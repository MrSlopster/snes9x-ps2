#pragma once
/* audsrv sound driver for the PS2 port, implementing the modern
 * S9xSoundDriver interface from common/audio/s9x_sound_driver.hpp. */

#include "../common/audio/s9x_sound_driver.hpp"

class S9xAudsrvSoundDriver : public S9xSoundDriver
{
    public:
    S9xAudsrvSoundDriver() = default;
    ~S9xAudsrvSoundDriver() override = default;

    void init() override;
    void deinit() override;
    bool open_device(int playback_rate, int buffer_size) override;
    void start() override;
    void stop() override;
    bool write_samples(int16_t *data, int samples) override;
    int space_free() override;
    std::pair<int, int> buffer_level() override;

    private:
    int rate = 0;
};
