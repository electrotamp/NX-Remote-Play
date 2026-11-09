/* Scoped access to the process service-manager connection. */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet
{

namespace sm_use
{
inline constinit os::SdkMutex g_lock;
} // namespace sm_use

class SmUse
{
  public:
    SmUse()
    {
        std::scoped_lock lk(sm_use::g_lock);
        m_ok = R_SUCCEEDED(::smInitialize());
    }
    ~SmUse()
    {
        if (!m_ok)
        {
            return;
        }
        std::scoped_lock lk(sm_use::g_lock);
        ::smExit();
    }
    SmUse(const SmUse &) = delete;
    SmUse &operator=(const SmUse &) = delete;
    bool ok() const
    {
        return m_ok;
    }

  private:
    bool m_ok;
};

} // namespace ams::mitm::applet
