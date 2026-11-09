Atmosphere patch used by the host (libstratosphere)

nxrp-libstratosphere.patch is a small change (18 lines added, 2 removed) to ONE Atmosphere header,
libraries/libstratosphere/include/stratosphere/sf/impl/sf_impl_command_serialization.hpp.
It lets a mitm service return an object whose session is forwarded to the real service (a side channel through the thread-local
g_nxrp_pending_mitm_forward) and lets the module supply its own session creator (g_nxrp_create_session, unset by default, which
keeps Atmosphere's normal behaviour).

Tested base: Atmosphere commit 6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8. Check that it applies:
  git apply --check nxrp-libstratosphere.patch      (run inside an Atmosphere checkout)
Apply: git apply nxrp-libstratosphere.patch
Build.ps1 at the repository root downloads the newest Atmosphere (or that commit with -Pinned, or if the newest does not build), applies this
patch and builds the library. If you use your own checkout with -AtmospherePath, apply the patch and run "make nx_release" in
libraries/libstratosphere first: the library contains code compiled from this header, and the host build does not rebuild it.
