# PotPlayer Discord Rich Presence

A Windhawk mod that adds Discord Rich Presence support to PotPlayer.

It displays the currently playing media in Discord, including the media title,
playback timestamps, PotPlayer branding, and optional TMDB artwork.

## Features

- 🎬 Displays the currently playing PotPlayer media in Discord
- ⏱️ Shows elapsed playback time
- ⏳ Shows the expected end time
- 🖼️ Uses TMDB posters when available
- 📺 Supports movies and TV shows through TMDB search
- 🎵 PotPlayer icon as the small Discord Rich Presence image
- 🔎 Cleans common video filename/release metadata
- 🔄 Detects seeking and updates timestamps
- 🌐 TMDB language configuration
- ⚙️ Configurable through Windhawk settings


## Screenshots

<img width="355" height="500" alt="image" src="https://github.com/user-attachments/assets/fb6ca6f2-a4a9-4684-b78a-dfa5ac188900" />


## Requirements

- Windows 10/11
- PotPlayer
- Discord desktop application
- Windhawk
- A Discord Developer Application
- Optional: TMDB API Read Access Token

## Installation

1. Install [Windhawk](https://windhawk.net/).
2. Download the `.wh.cpp` file from this repository.
3. Open Windhawk.
4. Create a new mod.
5. Paste the contents of the `.wh.cpp` file.
6. Compile and install the mod.
7. Configure the Discord Application ID.
8. Configure the TMDB token if artwork is desired.

## Discord Setup

Create a Discord application in the Discord Developer Portal.

Copy the application's Client ID and enter it into the Windhawk
mod's `Discord Application ID` setting.

### PotPlayer Icon

The mod uses a Discord Rich Presence asset for the PotPlayer icon.

Create an asset in your Discord application's Rich Presence assets
with the key:

`potplayer`

The key must match the value configured in the Windhawk settings.

## TMDB Setup

TMDB artwork is optional.

Create a TMDB account and obtain a TMDB API Read Access Token.

Enter the token into:

`TMDB API Read Access Token`

The mod searches TMDB using the cleaned PotPlayer title and attempts
movie and TV searches.

## Configuration

| Setting | Description |
|---|---|
| Discord Application ID | Discord Developer Application Client ID |
| TMDB API Read Access Token | TMDB Bearer token used for artwork searches |
| PotPlayer Small Image Asset | Discord asset key for the PotPlayer icon |
| TMDB Language | Language used for TMDB searches |

## Example

When playing a movie in PotPlayer, Discord can display:

**Watching**

`Movie Title`

with:

- TMDB poster
- PotPlayer icon
- elapsed playback time
- playback end time

## Limitations

- TMDB artwork depends on a successful title match.
- Filename/title matching is not guaranteed to identify the correct movie
  or TV show.
- The Discord desktop application must be running for local Rich Presence.
- Some PotPlayer playback states may not be detectable through the
  available PotPlayer messaging interface.

## License

See [LICENSE](LICENSE).
