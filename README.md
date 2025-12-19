# WMCPP (Wii Mandelbrot Computation Project Plus)

## Description

WMCPP (Wii Mandelbrot Computation Project Plus) is a continuation fork of
Krupkat's Mandelbrot set generator for Wii featuring real-time zooming,
customizable palettes, and precise fractal exploration. The Mandelbrot set,
popularized in the 1970s by mathematician Benoit Mandelbrot, is defined by
complex numbers that do not escape to infinity under iterative calculations.
Graphically rendered, the result is an intricate fractal pattern that can be
infinitely zoomed into, revealing complex and beautiful images. While Mandelbrot
generators have appeared on many platforms over the years, this version is
designed specifically for Wii via homebrew, allowing users to interactively
explore the fractal using a Wii Remote.

## Features

- Real-time zooming into the Mandelbrot set using a Wii Remote
- Adjustable color palettes with cycling options
- Configurable maximum iterations for higher precision rendering
- Exit functionality using the HOME button on the Wii Remote

## Controls

| **Wii Remote Action**  | **Function**                     |
|------------------------|----------------------------------|
| Aim                    | Point at where to zoom           |
| A Button               | Zoom in                          |
| B Button               | Start over                       |
| - / + Buttons          | Cycle through color palettes     |
| D-Pad Down             | Run cycling palette              |
| 1 / 2 Buttons          | Change the number of iterations  |
| HOME Button            | Exit                             |

## How to Build

### Prerequisites

- **devkitPro**
  - devkitPPC
  - libogc
  - wii-dev

### PowerPC devkitPro devkitPPC Toolchain and Build System Setup

To set up the devkitPro PowerPC devkitPPC toolchain and build system, follow the
instructions on the official devkitPro wiki:

- [Getting Started with devkitPro](https://devkitpro.org/wiki/Getting_Started)
- [DevkitPro Pacman](https://devkitpro.org/wiki/devkitPro_pacman)

## Build Instructions

1. Install `devkitPPC` with the necessary dependencies.
2. Navigate to the project directory.
3. Run `make` to compile the project. This will generate the `.elf` and `.dol`
   files for the Wii.

## How to Use

1. Rename the compiled `.dol` file to `boot.dol` and place it in an `apps/WMCPP`
   folder on your SD card.
2. Insert the SD card into your Wii.
3. Launch the Homebrew Channel, and select the WMCPP application.
4. Use the Wii Remote to zoom in and explore the Mandelbrot set, cycling
   through palettes and changing iterations for more detailed images.
5. Exit the application using the HOME button.

<br>

## Screenshots

![Mandelbrot Wii Screenshot](https://github.com/DeltaResero/WMCPP/blob/main/extras/Mandelbrotwii-screenshot_4.png?raw=true)

<br>

## Credits and License

WMCPP is developed by DeltaResero and is a derivative work based on the original
Mandelbrot for Wii project created by Krupkat in 2011 (originally released as
unspecified open source). Additional color palette data includes creative
contributions from Daniel Egnar, Tom Schumm, and Jussi Kantola.

This project is licensed under the **GNU General Public License v3 (GPLv3)** to
ensure that all modifications and forks remain open source. You are free to
redistribute and/or modify this program under the terms of the GPLv3 as
published by the Free Software Foundation.

## Disclaimer

This project is a homebrew application for the Wii and is not affiliated with,
endorsed by, nor sponsored by Nintendo. All trademarks are the property of
their respective owners.

This program is distributed **WITHOUT ANY WARRANTY**; without even the implied
warranty of **MERCHANTABILITY** or **FITNESS FOR A PARTICULAR PURPOSE**. See
the [GNU General Public License](https://www.gnu.org/licenses/gpl-3.0.en.html)
for more details.
