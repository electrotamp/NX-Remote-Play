using System.Security.AccessControl;
using System.Security.Principal;

namespace Rendezvous;

// The ordinary executable launch uses the same protected state as the setup
// helper. Enrollment remains local/trusted; pairing traffic cannot add keys.
internal static class DefaultServerSetup
{
    internal static string Prepare(string directory)
    {
        Directory.CreateDirectory(directory);
        if (OperatingSystem.IsWindows())
        {
            var sid = WindowsIdentity.GetCurrent().User?.Value
                ?? throw new IOException("Could not identify the server account.");
            var acl = new DirectorySecurity();
            acl.SetSecurityDescriptorSddlForm($"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;{sid})", AccessControlSections.Access);
            new DirectoryInfo(directory).SetAccessControl(acl);
        }
        string registry = Path.Combine(directory, "owners.json");
        string enrollment = Path.Combine(directory, "owner-enrollment.key");
        if (File.Exists(enrollment)) TrustedEnrollment.Import(registry, enrollment);
        return registry;
    }

    internal static string DirectoryPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "NX-Remote-Play", "Rendezvous");
}
