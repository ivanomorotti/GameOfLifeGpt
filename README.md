# GameOfLifeGpt

Interactive Conway's Game of Life implementation written in portable POSIX C. The engine keeps track of an effectively unbound grid of live cells and provides an interactive terminal renderer that supports panning and zooming during the simulation.

## Features

- Adjustable evolution speed via command-line flag.
- Optional configuration file loader to seed the board with a textual pattern.
- Interactive terminal controls for panning, zooming, pausing, and stepping.
- Optional SDL2 graphical renderer with a dark grid and white live cells.
- Hash-based sparse grid representation that allows the universe to grow without bounds.

## Build

A standard POSIX toolchain with `gcc` or `clang` is sufficient. SDL2 development headers are required for the graphical mode.
Compile the project with:

```sh
gcc -std=c11 -Wall -Wextra -pedantic src/main.c $(sdl2-config --cflags --libs) -o gameoflifegpt
```

## Usage

```
./gameoflifegpt [-t delay_ms] [-f file] [-g]
```

- `-t delay_ms` &mdash; milliseconds to wait between generations (default: 200).
- `-f file` &mdash; path to a pattern file to load before starting the simulation.
- `-g` &mdash; launch the SDL2 graphical renderer instead of the terminal UI.

### Controls

Once running, use the following keys inside the terminal window or SDL2 window:

- `q` &mdash; quit the program.
- `p` &mdash; pause or resume the automatic evolution.
- `n` &mdash; advance a single generation while paused.
- `w`, `a`, `s`, `d` &mdash; pan the view up, left, down, and right.
- `+`/`=` &mdash; zoom in on the current focus.
- `-` &mdash; zoom out to show more of the universe.
- `r` &mdash; reset the view to the origin.

The terminal renderer automatically adapts to the size of the window. Two additional lines beneath the grid display statistics and reminders of the available controls. In graphical mode, the window title shows the same information while the board is drawn with white squares over a dark grid.

### Configuration Files

Configuration files are plain text grids. Every `O`, `X`, `1`, or `o` character is treated as a live cell. Any other character (including `.` and spaces) is ignored and considered dead. Lines beginning with `!` or `#` are treated as comments and skipped. The top-left cell of the file is loaded at coordinate `(0, 0)`.

Example (`glider.txt`):

```
.O.
..O
OOO
```

Run the glider with:

```sh
./gameoflifegpt -f glider.txt -t 150
```

## License

This project is released into the public domain. Use it however you like.
