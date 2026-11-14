# Start screen banner

The app's start screen reads `banner.json` from this folder on the `main` branch
(`https://raw.githubusercontent.com/mamcjr/BOTWRecompAndroid/main/banner/banner.json`) each time it opens, and shows
the image it names above the version, in the lower right. No new app release is needed to put one up or take it down:

- **Show one:** add the PNG here and point `image` at it, `"enabled": true`, commit and push to `main`.
- **Take it down:** `"enabled": false` (or delete `banner.json`), commit and push. GitHub's cache can take ~5 minutes.

Fields:

| Field | |
|---|---|
| `enabled` | `true` / `false` |
| `image` | a PNG in this folder (`"thank-you-800-stars.png"`) or a full `https://` URL. Letters, digits, `.`, `-`, `_` only. The app keeps it under this name and downloads it again only when the name changes, so **a new picture needs a new file name**. At most 4 MB; ~1120 px wide is plenty (16:9 looks best). |
| `link` | optional: `https://` page opened when the banner is tapped |
| `start`, `end` | optional: `"YYYY-MM-DD"`, the first and last day it shows (the phone's local date) |

Offline, no `banner.json`, a bad image or outside the dates: nothing is shown, and the start screen never waits for
the network (the download runs in the background; the banner appears when it's ready).
