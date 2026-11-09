Memory donor (Switch sysmodule, built with devkitPro/libnx)

A tiny program the host starts while a viewer is connected. It asks the host's control service for a memory size, receives
that memory from the system's Applet pool and keeps it until the stream stops, then exits.

Built by Build.ps1 at the repository root (-Only Donor builds just this), in a scratch copy under Build\donor. It never installs or starts the donor.
Output: <name>.nsp. Install it as atmosphere\contents\<program ID>\exefs.nsp (no boot2 flag: the host launches it).
The program ID in donor.json (4E5852656D4D656D, "NXRemMem") must match the ID the host launches (DonorProgramId in
Source\Host\source\nxrp_video\applet_mitm_donor.cpp), and the service in service_access (nxrpvid) must match the host's
control service name.
