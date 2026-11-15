"""Exercise committed viewport crops, scaling, protocol errors, and lifetime."""

import os
from pathlib import Path
import signal
import struct
import sys
import tempfile

from host_support import connect_x11, server
from native_lifecycle import wait_socket
from native_shm import create_pool, create_buffer, SHM_ARGB8888
from native_surfaces import root_window, query_tree, create_surface
from native_transforms import (PIXELS, TRANSFORM_COUNT, SURFACE_TRANSFORM,
                               SURFACE_SCALE, SURFACE_ATTACH, SURFACE_COMMIT,
                               attach_rows, transformed, wait_image)
from native_xdg import XdgClient
from wayland_support import Wayland, WaylandProtocolError

DESTROY = 0
GET_VIEWPORT = 1
SET_SOURCE = 1
SET_DESTINATION = 2
FIXED_ONE = 256
UNSET_SOURCE = (-1, -1, -1, -1)
UNSET_DESTINATION = (-1, -1)
VIEWPORT_EXISTS = 0
BAD_VALUE = 0
BAD_SIZE = 1
OUT_OF_BUFFER = 2
NO_SURFACE = 3
INT32_MAX = (1 << 31) - 1


def create_viewport(client, porter, surface):
    viewport = client.allocate_id()
    client.send(porter, GET_VIEWPORT, struct.pack("=II", viewport, surface))
    return viewport


def source(client, viewport, rectangle):
    client.send(viewport, SET_SOURCE,
                struct.pack("=4i", *(int(value * FIXED_ONE) for value in rectangle)))


def destination(client, viewport, size):
    client.send(viewport, SET_DESTINATION, struct.pack("=ii", *size))


def check_crops(path, x11, root):
    before = query_tree(x11, root)
    client = XdgClient(path)
    try:
        window, = query_tree(x11, root) - before
        porter = client.bind("wp_viewporter")
        viewport = create_viewport(client, porter, client.surface)
        # Bound global destruction preserves the viewport it created.
        client.send(porter, DESTROY)
        client.commit()
        client.configure()
        cropped = tuple(row[1:] for row in PIXELS)
        for transform in range(TRANSFORM_COUNT):
            for scale in (1, 2):
                client.send(client.surface, SURFACE_TRANSFORM, struct.pack("=i", transform))
                client.send(client.surface, SURFACE_SCALE, struct.pack("=i", scale))
                source(client, viewport, (1, 0, 2, 2))
                destination(client, viewport, (4, 4))
                fd, buffer = attach_rows(client, transformed(PIXELS, transform, scale))
                try:
                    client.commit()
                    assert (buffer, 0, b"") in client.roundtrip_events()
                    client.send(buffer, DESTROY)
                finally:
                    os.close(fd)
                wait_image(x11, window, transformed(cropped, 0, 2))
                destination(client, viewport, UNSET_DESTINATION)
                client.sync()
                wait_image(x11, window, transformed(cropped, 0, 2))
                client.commit()
                client.sync()
                wait_image(x11, window, cropped)
                source(client, viewport, (0.125, 0.125, 1.5, 0.75))
                destination(client, viewport, (3, 3))
                client.commit()
                client.sync()
                wait_image(x11, window, ((PIXELS[0][0], PIXELS[0][0], PIXELS[0][1]),) * 3)
                source(client, viewport, UNSET_SOURCE)
                destination(client, viewport, UNSET_DESTINATION)
                client.commit()
                client.sync()
                wait_image(x11, window, PIXELS)
        # Source and destination state survive commits that attach no new buffer.
        source(client, viewport, (1, 0, 2, 2))
        client.commit()
        client.sync()
        wait_image(x11, window, cropped)
        client.send(viewport, DESTROY)
        client.sync()
        wait_image(x11, window, cropped)
        client.commit()
        client.sync()
        wait_image(x11, window, PIXELS)
        viewport = create_viewport(client, client.bind("wp_viewporter"), client.surface)
        destination(client, viewport, (6, 4))
        client.commit()
        client.sync()
        wait_image(x11, window, transformed(PIXELS, 0, 2))
        # Destroy pending state before it becomes current.
        source(client, viewport, (100, 100, 1, 1))
        client.send(viewport, DESTROY)
        client.commit()
        client.sync()
        wait_image(x11, window, PIXELS)
    finally:
        client.close()


def check_error(path, failure):
    client = Wayland(path)
    try:
        surface = create_surface(client, client.bind("wl_compositor"))
        porter = client.bind("wp_viewporter")
        viewport = create_viewport(client, porter, surface)
        target, code = viewport, BAD_VALUE
        commit = False
        if failure == "duplicate":
            create_viewport(client, porter, surface)
            target, code = porter, VIEWPORT_EXISTS
        elif failure == "negative":
            source(client, viewport, (-0.5, 0, 1, 1))
        elif failure == "zero":
            source(client, viewport, (0, 0, 0, 1))
        elif failure == "mixed-source":
            source(client, viewport, (-1, -1, -1, 1))
        elif failure == "mixed-destination":
            destination(client, viewport, (-1, 1))
        elif failure == "destination":
            destination(client, viewport, (0, 1))
        elif failure == "fractional":
            source(client, viewport, (0, 0, 1.5, 1))
            code, commit = BAD_SIZE, True
        elif failure in ("outside", "overflow", "changed-buffer"):
            fd = os.memfd_create("meatball-viewport-error", os.MFD_CLOEXEC)
            try:
                os.ftruncate(fd, 8)
                pool = create_pool(client, client.bind("wl_shm"), fd, 8)
                buffer = create_buffer(client, pool, 0, 2, 1, 8, SHM_ARGB8888)
                client.send(surface, SURFACE_ATTACH, struct.pack("=Iii", buffer, 0, 0))
                if failure == "overflow":
                    destination(client, viewport, (1, 1))
                    client.send(viewport, SET_SOURCE,
                                struct.pack("=4i", INT32_MAX, 0, INT32_MAX, FIXED_ONE))
                elif failure == "changed-buffer":
                    source(client, viewport, (1, 0, 1, 1))
                    client.send(surface, SURFACE_COMMIT)
                    client.sync()
                    smaller = create_buffer(client, pool, 0, 1, 1, 4, SHM_ARGB8888)
                    client.send(surface, SURFACE_ATTACH, struct.pack("=Iii", smaller, 0, 0))
                else:
                    source(client, viewport, (1.5, 0, 1, 1))
            finally:
                os.close(fd)
            code, commit = OUT_OF_BUFFER, True
        elif failure in ("no-source-surface", "no-destination-surface"):
            client.send(surface, DESTROY)
            if failure == "no-source-surface":
                source(client, viewport, UNSET_SOURCE)
            else:
                destination(client, viewport, UNSET_DESTINATION)
            code = NO_SURFACE
        else:
            raise AssertionError(failure)
        if commit:
            # Validation of the completed state belongs to surface commit.
            client.sync()
            client.send(surface, SURFACE_COMMIT)
        try:
            client.sync()
            raise AssertionError(f"invalid viewport accepted: {failure}")
        except WaylandProtocolError as error:
            assert (error.object_id, error.code) == (target, code), (failure, str(error))
    finally:
        client.close()


def main():
    host, module = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="meatball-viewport-") as directory:
        path = Path(directory) / "wayland"
        with server(host, module, {"MEATBALL_TEST_SOCKET": str(path)}) as (process, display):
            wait_socket(path, process)
            x11, setup = connect_x11(display, return_setup=True)
            try:
                root = root_window(setup)
                check_crops(path, x11, root)
                observer = Wayland(path)
                try:
                    for failure in ("duplicate", "negative", "zero", "mixed-source",
                                    "mixed-destination", "destination", "fractional",
                                    "outside", "overflow", "changed-buffer",
                                    "no-source-surface", "no-destination-surface"):
                        check_error(path, failure)
                        observer.sync()
                finally:
                    observer.close()
                client = Wayland(path)
                surface = create_surface(client, client.bind("wl_compositor"))
                viewport = create_viewport(client, client.bind("wp_viewporter"), surface)
                source(client, viewport, (100, 100, 1, 1))
                client.send(surface, SURFACE_COMMIT)
                client.sync()  # A surface without content permits an out-of-bounds source.
                process.send_signal(signal.SIGHUP)
                assert x11.recv(1) == b""
                client.disconnected()
                client.close()
            finally:
                x11.close()
            with connect_x11(display):
                client = Wayland(path)
                surface = create_surface(client, client.bind("wl_compositor"))
                viewport = create_viewport(client, client.bind("wp_viewporter"), surface)
                client.send(surface, DESTROY)
                client.send(viewport, DESTROY)
                client.sync()
                client.close()


if __name__ == "__main__":
    main()
