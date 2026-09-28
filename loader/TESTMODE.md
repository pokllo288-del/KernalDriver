# Loading the driver UNSIGNED — enable Test Mode (super simple)

Our driver is **not signed**, so normal Windows refuses to load it. Windows has
a built-in developer switch — **Test Signing Mode** — that lets *your own*
machine load unsigned/test drivers. Do this **only on a test VM you own.**

## The 3 steps

**1. Open an Administrator command prompt**
(Start → type `cmd` → right-click → *Run as administrator*.)

**2. Turn on Test Mode**
```
bcdedit /set testsigning on
```
You should see `The operation completed successfully.`
(Starlite has an **"Enable Test Mode"** button that runs this for you.)

**3. Reboot.**
After it reboots you'll see a small **"Test Mode"** watermark in the bottom-right
corner of the desktop — that means it worked. Now the driver will load.

To turn it back off later:
```
bcdedit /set testsigning off
```
(then reboot; the watermark disappears).

## If step 2 fails

`bcdedit` fails with an "element data" / "secure boot" error when **Secure Boot**
is enabled. Fix:
1. Reboot into your **UEFI/BIOS** (usually Del or F2 at power-on; in a VM, use
   the VM's firmware settings).
2. **Disable Secure Boot**, save, exit.
3. Run step 2 again.

## One-time alternative (no permanent Test Mode)

If you'd rather not leave Test Mode on, you can allow unsigned drivers for a
**single boot**:

1. Settings → System → Recovery → **Advanced startup → Restart now**
   (or hold **Shift** while clicking Restart).
2. **Troubleshoot → Advanced options → Startup Settings → Restart**.
3. Press **7** (or **F7**) — *Disable driver signature enforcement*.

Windows boots once with enforcement off; it returns to normal on the next
reboot.

---

⚠️ Test Mode / disabling enforcement lowers a security protection. Only do it on
a dedicated test machine or VM, never on a daily-driver PC. This is the standard
driver-development workflow — it is **not** a way around anyone else's security.
