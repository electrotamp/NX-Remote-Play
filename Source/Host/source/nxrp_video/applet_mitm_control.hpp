// Memory-donor service. Command IDs are shared with Source/Donor.
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet
{

void StartControlService();
} // namespace ams::mitm::applet

#define AMS_NXRP_CONTROL_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 10, Result, Donate, (u64 size, sf::CopyHandle &&tmem), (size, std::move(tmem))) \
    AMS_SF_METHOD_INFO(C, H, 11, Result, DonateFailed, (u32 rc), (rc)) \
    AMS_SF_METHOD_INFO(C, H, 12, Result, Donate2, (u64 size, u64 nv_size, sf::CopyHandle &&tmem, sf::CopyHandle &&nv_tmem), (size, nv_size, std::move(tmem), std::move(nv_tmem))) \
    AMS_SF_METHOD_INFO(C, H, 13, Result, GetDonorCap, (sf::Out<u32> out_mb), (out_mb))

AMS_SF_DEFINE_INTERFACE(ams::mitm::applet, IStreamControl, AMS_NXRP_CONTROL_INTERFACE_INFO, 0x5F7A4001)
