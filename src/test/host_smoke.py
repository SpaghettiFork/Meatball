"""Exercise the required SDK exports in an isolated Spaghetti server."""

import sys
from host_support import connect_x11, server


def main():
    with server(*sys.argv[1:3]) as (_, display):
        with connect_x11(display):
            pass


if __name__ == "__main__":
    main()
