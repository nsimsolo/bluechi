#
# Copyright Contributors to the Eclipse BlueChi project
#
# SPDX-License-Identifier: LGPL-2.1-or-later

import logging
import random
import re
import signal
import socket
import string
from typing import Union

LOGGER = logging.getLogger(__name__)


def read_file(local_file: str) -> Union[str, None]:
    content = None
    with open(local_file, "r") as fh:
        content = fh.read()
    return content


def get_primary_ip(address: str = "254.254.254.254") -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0)
    try:
        # connect to an arbitrary address
        s.connect((address, 1))
        ip = s.getsockname()[0]
    except Exception:
        LOGGER.exception("Failed to get IP, falling back to localhost.")
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip


def assemble_bluechi_dep_service_name(unit_name: str) -> str:
    return f"bluechi-dep@{unit_name}"


def assemble_bluechi_proxy_service_name(node_name: str, unit_name: str) -> str:
    return f"bluechi-proxy@{node_name}_{unit_name}"


def get_random_name(name_length: int) -> str:
    # choose from all lowercase letter
    letters = string.ascii_lowercase
    return "".join(random.choice(letters) for _ in range(name_length))


_ANSI_SEQUENCE = re.compile(
    r"""
    \x1B  # ESC
    (?:   # 7-bit C1 Fe (except CSI)
        [@-Z\\-_]
    |     # or [ for CSI, followed by a control sequence
        \[
        [0-?]*  # Parameter bytes
        [ -/]*  # Intermediate bytes
        [@-~]   # Final byte
    )
""",
    re.VERBOSE,
)


def remove_control_chars(value: str) -> str:
    return _ANSI_SEQUENCE.sub("", value)


class Timeout:
    def __init__(self, seconds=1, error_message="Timeout"):
        self.seconds = seconds
        self.error_message = error_message

    def handle_timeout(self, signum, frame):
        raise TimeoutError(self.error_message)

    def __enter__(self):
        signal.signal(signal.SIGALRM, self.handle_timeout)
        signal.alarm(self.seconds)

    def __exit__(self, type, value, traceback):
        signal.alarm(0)
