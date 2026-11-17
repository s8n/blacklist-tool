# blacklist-tool

blacklist-tool lets you view and modify the [FunctionBlackList](https://switchbrew.org/wiki/Shared_Database_services#FunctionBlackList) on a Switch 2 console.

This can be used to:
- see which games have been blocked
- unblock games that have mandatory updates, such as Zelda: TotK, so you can play copies of games you own without having to update them.
- install a completely custom blacklist, so you can block certain games from being booted, or from certain functionality of games from being used (see the switchbrew link above).

You're solely responsible for any bugs or issues you might run into.

## Usage

- (Boot to something like kaiHEN. You can transfer the file over FTP later.)
- Download .nro from [Releases](https://github.com/s8n/blacklist-tool/releases), and copy on your Switch 2 microSD's /switch folder.
- Open the blacklist-tool in homebrew menu.
- You can press left and right to select different options.
- If you modify your blacklist, reboot afterwards.

## Background

`sdb` reads `blacklist.json` from two places and uses the copy with the higher
top-level `version`. The BCAT version is only used when it is higher, else system data is used.

| Source | Title ID | Updated by |
| --- | --- | --- |
| System data (`FunctionBlackList`) | `0400000000000828` | System updates, including rebootless updates |
| BCAT delivery cache (`blacklist/blacklist.json`) | `040000000000102E` | bcat downloads, with no system update |

We can list these for documentation purposes, and overwrite them for any reason, e.g. to unblock games we lack updates for.

## SD files in `sdmc:/switch/blacklist-tool/`

Timestamps use `YYYYMMDD-HHMMSS`.

| File | Purpose |
| --- | --- |
| `payload.json` | Optional. If present, Write functionality writes this file instead of an empty list |
| `bcat-original-blacklist.json` | The BCAT copy saved the first time you overwrite the blacklist; later runs leave it unchanged |
| `bcat-created.txt` | Present when Write Empty created `blacklist.json` in a BCAT save that had none |
| `<timestamp>-sysdata-blacklist.json` | System data file written by Dump |
| `<timestamp>-bcat-blacklist.json` | BCAT copy from `bcat:s` written by Dump |
| `<timestamp>-bcat-save-blacklist.json` | BCAT copy from the save written by Dump |
| `<timestamp>-report.txt` | The diagnostic readout at the time of the dump |
| `<timestamp>-bcat-save/` | A full copy of the BCAT delivery cache save, written by Write functionality before it changes anything |

See the information on switchbrew to see how you can make your own blacklist.json, or see examples in `payloads/`.

## Warning

Writing the BCAT copy may lead to a console ban if Nintendo wants to do so. They may also not ban you. As with all homebrew, you assume the risk.

Do not blame anyone but yourself if that happens. Complaining about bans will get you banned from this github org. PRs to note that you got banned just for using this are fine.

Beware that changes through this persist reboots, and possibly even factory resets.

## Building

```sh
./build.sh          # Switch 1, writes blacklist-tool.nro, mainly useful for testing (blacklist isn't used on Switch 1)
./build.sh switch2  # Switch 2, writes blacklist-tool-switch2.nro
```

## Thanks

- Syboxes and fryskii, for testing it!
