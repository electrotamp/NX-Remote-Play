// VIC BT.709 RGB-to-NV12 conversion coefficients.
#pragma once

namespace ams::mitm::applet::vic_csc
{

constexpr u32 Shift = 8;
constexpr s32 Bt709[3][4] = {
    {4064, 40254, 11966, 16896},
    {-2639, -26145, 28784, 131584},
    {28785, -22189, -6596, 131584},
};

} // namespace ams::mitm::applet::vic_csc
