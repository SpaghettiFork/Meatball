"""Check single-pixel colors, viewport scaling, and buffer lifetime."""

from pathlib import Path
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_lifecycle import wait_socket
from native_subsurfaces import child, commit, new_client, rectangle, solid
from native_surfaces import query_tree, root_window
from native_transforms import SURFACE_ATTACH, SURFACE_TRANSFORM, wait_image
from native_viewport import create_viewport, destination, source

INTERFACE = "wp_single_pixel_buffer_manager_v1"
CREATE_BUFFER = 1
DESTROY = 0
RELEASE = 0
CHANNEL_MAX = (1 << 32) - 1
HALF_CHANNEL = 1 << 31
ROTATE_90 = 1
RED = 0xffff0000
HALF_GREEN_OVER_RED = 0xff7f8000


def create_pixel(client, manager, rgba):
    buffer = client.allocate_id()
    client.send(manager, CREATE_BUFFER, struct.pack("=IIIII", buffer, *rgba))
    return buffer


def check_pixels(path, x11, root):
    before = query_tree(x11, root)
    client = new_client(path)
    try:
        window, = query_tree(x11, root) - before
        manager = client.bind(INTERFACE, 1)
        red = create_pixel(client, manager, (CHANNEL_MAX, 0, 0, CHANNEL_MAX))
        green = create_pixel(client, manager, (0, HALF_CHANNEL, 0, HALF_CHANNEL))
        transparent = create_pixel(client, manager, (0, 0, 0, 0))
        client.send(manager, DESTROY)

        client.commit()
        client.configure()
        for _ in range(2):
            client.attach(red)
            client.commit()
            assert client.roundtrip_events().count((red, RELEASE, b"")) == 1
            wait_image(x11, window, solid(RED, 1, 1))
        client.send(red, DESTROY)

        porter = client.bind("wp_viewporter")
        viewport = create_viewport(client, porter, client.surface)
        source(client, viewport, (0.25, 0, 0.5, 1))
        destination(client, viewport, (4, 3))
        client.send(client.surface, SURFACE_TRANSFORM, struct.pack("=i", ROTATE_90))
        client.commit()
        client.sync()
        background = solid(RED, 4, 3)
        wait_image(x11, window, background)

        overlay, _ = child(client, client.surface, 1, 1)
        overlay_viewport = create_viewport(client, porter, overlay)
        destination(client, overlay_viewport, (2, 2))
        client.send(overlay, SURFACE_ATTACH, struct.pack("=Iii", green, 0, 0))
        commit(client, overlay)
        assert (green, RELEASE, b"") in client.roundtrip_events()
        client.send(green, DESTROY)
        wait_image(x11, window, background)
        client.commit()
        client.sync()
        wait_image(x11, window, rectangle(background, HALF_GREEN_OVER_RED, 1, 1, 2, 2))

        client.send(overlay, SURFACE_ATTACH, struct.pack("=Iii", transparent, 0, 0))
        client.send(transparent, DESTROY)
        commit(client, overlay)
        client.commit()
        client.sync()
        wait_image(x11, window, background)
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-single-pixel-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            with x11:
                check_pixels(path, x11, root_window(setup))
            assert process.poll() is None


if __name__ == "__main__":
    main()
