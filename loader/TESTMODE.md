# Make the unsigned driver load — quick instructions

The driver is **not signed**, so Windows blocks it by default. To run it on
**your own test machine / VM**, turn on **Test Mode** once:

1. Open **Command Prompt as Administrator**.
2. Run:
   ```
   bcdedit /set testsigning on
   ```
3. **Reboot.** After restart you'll see a small **"Test Mode"** watermark
   (bottom-right of the desktop) — that means it's active.
4. Launch **Starlite.exe** (as Administrator) and click **LOAD**.

To turn it back off later: `bcdedit /set testsigning off`, then reboot.

### If step 2 says it failed / mentions Secure Boot
Secure Boot must be off. Reboot into your **UEFI/BIOS**, **disable Secure Boot**,
save & exit, then run step 2 again.

---
⚠️ Only do this on a machine/VM you own and use for testing. Test Mode lowers a
security protection; it's the normal driver-development workflow, not a way
around anyone else's system.
