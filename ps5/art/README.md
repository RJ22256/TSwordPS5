# The home screen: your icon, backgrounds and music

Everything the PS5 home screen shows for TSword comes from this folder. Each build turns it into the formats the
console takes (`ps5/tools/home-screen.sh`, using PS5_Vulkan's `prepare-assets.sh`). Replace a file here with your own
and build a release: GitHub > this folder > Add file > Upload files (same name, replacing it), then Actions > Build >
Run workflow with a release name. Or copy the finished files straight to the console (below).

| File here | What it is on the home screen | What it can be |
|---|---|---|
| `icon.png` | the app's tile | any image ffmpeg reads (PNG, JPG, WebP...), square: it becomes 512x512 |
| `background.png` | the backdrop while TSword is selected | any image, 16:9: it becomes 3840x2160 |
| `launch-background.png` | the backdrop while TSword starts (optional) | as above; without it, `background.png` again |
| `music.at9` | the music while TSword is selected (optional) | ATRAC9, up to 87 s, 48 kHz stereo |

The icon is stretched to a square and the backgrounds to 16:9, so make them that shape (or fill the edges yourself).

**Music** has to be ATRAC9 already. The PS5 plays nothing else there, and the only encoder is Sony's (the PlayStation
SDK's `at9tool`, which can't run in these builds). A music.at9 you're entitled to use goes in as it is.

**On the console directly** (no build): the finished files are in `/data/homebrew/PPSA99778/sce_sys/` - `icon0.png`
(512x512 PNG), `pic0.dds` (selected) and `pic1.dds` (starting): 3840x2160 BC7 DDS, and `snd0.at9`. Replace them over
FTP; the home screen may keep the old ones until the PS5 restarts.

The shipped `icon.png` and `background.png` are WSword's artwork with its blues turned green (the sky, the mountains,
the glow and the petals' accents; the hilt's violet and the leaves as they were).
