# Windows 11 — UF8 connects to Windows but not to SSL 360°

## Symptom
Device Manager events for UF8:
```
Device install requested
Device configured (oem9.inf)
Device started (SSLBUS)
Information: Device USB\VID_31E9&PID_0021\UF-001254 requires further installation.
```
SSL 360° shows the UF8 as disconnected / not found.

## Diagnosis
The **SSLBUS** filter/bus driver loaded successfully — it claimed the vendor-specific USB interface (`0xFF/0xFF/0xFF`) and enumerates child functional devices. "Requires further installation" means those child devices do not have their upper-level drivers (the functional SSL drivers) installed. Typical causes:

1. **SSL 360° installer was not run as Administrator** — driver INFs for child devices weren't accepted by Windows
2. **Installer was interrupted** or the OEM driver-signing prompt was dismissed
3. **Stale previous install** — a half-installed earlier SSL 360° version left conflicting INFs (oem9.inf is the current one, but older oemN.inf files may confuse things)
4. **UF8 firmware** too old for current SSL 360° build (unlikely on a fresh Windows, but possible)

## Fix — full clean reinstall (the reliable path)

### 1. Remove SSL 360° and all SSL drivers
- Settings → Apps → **Uninstall SSL 360°**
- Unplug the UF8
- Device Manager → View → **Show hidden devices** → find any remaining "SSL Control I/F" / "SSL USB" / UF8 entries → right-click → Uninstall device → **check "Delete driver software"**
- (optional but clean) `pnputil /enum-drivers | findstr /i "ssl"` in an admin shell, then `pnputil /delete-driver oemN.inf /uninstall /force` for each listed SSL INF
- Reboot

### 2. Download current SSL 360°
- https://www.solidstatelogic.com/support-page/uf8-downloads → latest Windows build (needs to be **6.3 or newer** for Windows 11)

### 3. Install as Administrator
- Right-click the installer → **Run as administrator**
- When Windows shows "Do you want to install this device software?" for Solid State Logic → **always Install**, never Don't Install
- Let the installer finish fully (UAC + multiple driver signing prompts)

### 4. Reconnect UF8
- Plug UF8 into a **direct USB port on the motherboard** (not a hub, not a monitor's USB, not a Thunderbolt dock)
- Device Manager should now show the UF8 under a new category (usually "Solid State Logic" or under "Universal Serial Bus devices") **without** the warning triangle and without "requires further installation"

### 5. Launch SSL 360° as Administrator the first time
- Right-click SSL 360° shortcut → Run as administrator
- It should now detect the UF8 and offer to register / authorize

### 6. Verify before capturing
- UF8 display shows meters or channel names → SSL 360° has connected
- In SSL 360°, load Plugin Mixer page, make sure at least one SSL Channel Strip is loaded on a REAPER track
- Move a fader or change a track color → UF8 display reacts → colors are flowing over USB → we're ready to capture

## If still "requires further installation" after the clean reinstall

Likely a driver-signing issue. In an **admin PowerShell**:
```powershell
Get-WindowsDriver -Online | Where-Object { $_.ProviderName -like "*Solid State*" }
```
to list installed SSL drivers. If the functional drivers (not SSLBUS, but the child-device drivers) are missing, the installer isn't placing them. Check Windows Event Viewer → Applications and Services Logs → Microsoft → Windows → DriverFrameworks-UserMode for the error cause.

Last-resort: Solid State Logic support — they have a "driver cleanup tool" they share on request.

## Reference — driver file names
- `oem9.inf` (or similar oemN.inf, number varies) = the SSL bus driver. This one loaded.
- There should be an additional `oemM.inf` for the functional devices that's currently **missing** in the failure case.
