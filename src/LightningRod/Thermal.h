#pragma once

#include <windows.h>

struct IWbemServices;

namespace tlr {

// Reads the ACPI thermal zone temperature through WMI (root\WMI,
// MSAcpi_ThermalZoneTemperature). The zone is whatever the board firmware
// reports; many desktop boards expose none. The calling thread must have
// initialised COM.
class ThermalReader {
public:
    ThermalReader();
    ~ThermalReader();
    // Hottest zone in tenths of a degree Celsius; false when no zone reports a value.
    bool read(int& tenthsC);
private:
    bool connect();
    void release();
    IWbemServices* services_;
};

} // namespace tlr
