/*
  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.
*/


// Kvaser CANlib (linuxcan's libcanlib, canlib32.dll of the Kvaser drivers on Windows) loaded at run time, so the Kvaser driver is always built
// and works wherever the library is installed, without the SDK at build time. Only what the driver
// uses is declared; the names, values and signatures are CANlib's public, stable API.

#pragma once

#include <cstddef>

using canHandle = int;
using canStatus = int;

inline constexpr canStatus canOK = 0;
inline constexpr canStatus canERR_NOMSG = -2;
inline constexpr canStatus canERR_TIMEOUT = -7;

inline constexpr int canOPEN_ACCEPT_VIRTUAL = 0x0020;
inline constexpr int canOPEN_CAN_FD = 0x0400;

inline constexpr int canCHANNELDATA_CHANNEL_CAP = 1;
inline constexpr int canCHANNELDATA_DEVDESCR_ASCII = 26;
inline constexpr unsigned canCHANNEL_CAP_CAN_FD = 0x00080000;

inline constexpr unsigned canDRIVER_NORMAL = 4;
inline constexpr unsigned canDRIVER_SILENT = 1;

inline constexpr unsigned canMSG_RTR = 0x0001;
inline constexpr unsigned canMSG_STD = 0x0002;
inline constexpr unsigned canMSG_EXT = 0x0004;
inline constexpr unsigned canMSG_ERROR_FRAME = 0x0020;
inline constexpr unsigned canFDMSG_FDF = 0x010000;
inline constexpr unsigned canFDMSG_BRS = 0x020000;

inline constexpr long canBITRATE_1M = -1;
inline constexpr long canBITRATE_500K = -2;
inline constexpr long canBITRATE_250K = -3;
inline constexpr long canBITRATE_125K = -4;
inline constexpr long canBITRATE_100K = -5;
inline constexpr long canBITRATE_62K = -6;
inline constexpr long canBITRATE_50K = -7;
inline constexpr long canBITRATE_83K = -8;
inline constexpr long canBITRATE_10K = -9;
inline constexpr long canFD_BITRATE_500K_80P = -1000;
inline constexpr long canFD_BITRATE_1M_80P = -1001;
inline constexpr long canFD_BITRATE_2M_80P = -1002;
inline constexpr long canFD_BITRATE_4M_80P = -1003;
inline constexpr long canFD_BITRATE_8M_80P = -1005;

inline constexpr unsigned canIOCTL_SET_TIMER_SCALE = 6; // buffer: uint32 microseconds per timestamp tick

inline constexpr unsigned long canSTAT_ERROR_PASSIVE = 0x01;
inline constexpr unsigned long canSTAT_BUS_OFF = 0x02;
inline constexpr unsigned long canSTAT_ERROR_WARNING = 0x04;

struct Canlib
{
    void (*canInitializeLibrary)();
    canStatus (*canGetNumberOfChannels)(int* count);
    canStatus (*canGetChannelData)(int channel, int item, void* buffer, std::size_t size);
    canHandle (*canOpenChannel)(int channel, int flags);
    canStatus (*canSetBusParams)(canHandle h, long freq, unsigned tseg1, unsigned tseg2, unsigned sjw, unsigned no_samp, unsigned sync);
    canStatus (*canSetBusParamsFd)(canHandle h, long freq_brs, unsigned tseg1, unsigned tseg2, unsigned sjw);
    canStatus (*canSetBusOutputControl)(canHandle h, unsigned driver_type);
    canStatus (*canBusOn)(canHandle h);
    canStatus (*canBusOff)(canHandle h);
    canStatus (*canClose)(canHandle h);
    canStatus (*canWrite)(canHandle h, long id, void* data, unsigned dlc, unsigned flags);
    canStatus (*canReadWait)(canHandle h, long* id, void* data, unsigned* dlc, unsigned* flags, unsigned long* time, unsigned long timeout);
    canStatus (*canRequestChipStatus)(canHandle h);
    canStatus (*canReadStatus)(canHandle h, unsigned long* flags);
    canStatus (*canIoCtl)(canHandle h, unsigned func, void* buffer, unsigned size);
};

// The library, loaded (dlopen "libcanlib.so.1", then "libcanlib.so"; "canlib32.dll" on Windows) and initialised on the
// first call; nullptr when it is not installed, which the driver reports as "no channels".
const Canlib* canlib_load();
