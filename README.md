# performance v0.1.0
On the line music conference

## Usage


## Development
Codename: 'work'

**To build + test:**
1. sudo apt install libopus-dev libspeexdsp-dev
2. gcc jam_ref.c -o jam -lopus -lspeexdsp -lpthread -lm -D_GNU_SOURCE
3. Terminal 1: ./jam 127.0.0.1 9000 9001
4. Terminal 2: echo quit | nc 127.0.0.1 9001 to stop
