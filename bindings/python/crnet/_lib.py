# SPDX-License-Identifier: BSD-3-Clause
"""ctypes declarations for libcrnet's C API (include/crnet/crnet.h).

Only the functions the Python module uses are declared. ctypes releases
the GIL around every call, so blocking calls do not stall other Python
threads.
"""

import ctypes
import os
from ctypes import (POINTER, c_char_p, c_int, c_int64, c_size_t, c_uint64,
                    c_void_p)

# The oldest library with every function declared below.
MIN_VERSION = (0, 26, 0)

FOREVER = 2**64 - 1     # UINT64_MAX: wait without a time limit


def _load():
    path = os.environ.get("CRNET_LIBRARY")
    names = [path] if path else ["libcrnet.so.0", "libcrnet.so"]
    errors = []
    for name in names:
        try:
            return ctypes.CDLL(name)
        except OSError as e:
            errors.append(str(e))
    raise ImportError(
        "cannot load libcrnet (set CRNET_LIBRARY to its path): " +
        "; ".join(errors))


lib = _load()

_P = c_void_p           # every crnet handle is opaque


def _fn(name, restype, *argtypes):
    try:
        f = getattr(lib, name)
    except AttributeError:
        raise ImportError(
            "libcrnet lacks %s; the Python module needs %d.%d.%d or newer"
            % ((name,) + MIN_VERSION)) from None
    f.restype = restype
    f.argtypes = list(argtypes)
    globals()[name] = f


# Library
_fn("crnet_version", c_char_p)
_fn("crnet_chromium_version", c_char_p)
_fn("crnet_strerror", c_char_p, c_int)
_fn("crnet_default_user_agent", c_char_p)
_fn("crnet_default_sec_ch_ua", c_char_p)
_fn("crnet_platform_user_agent", c_char_p, c_int)
_fn("crnet_global_init", c_int, c_char_p, c_char_p)
_fn("crnet_free", None, c_void_p)

# Client configuration
_fn("crnet_client_config_new", _P)
_fn("crnet_client_config_free", None, _P)
for _name in ("proxy", "proxy_bypass", "user_agent", "sec_ch_ua",
              "platform", "accept_language", "host_rules", "netlog",
              "state_dir", "local_address", "bind_interface"):
    _fn("crnet_client_config_set_" + _name, c_int, _P, c_char_p)
for _name in ("http2", "quic", "builtin_dns", "cookies", "insecure",
              "trust_anchors_only"):
    _fn("crnet_client_config_set_" + _name, None, _P, c_int)
_fn("crnet_client_config_set_platform_profile", c_int, _P, c_int)
_fn("crnet_client_config_set_secure_dns", c_int, _P, c_int, c_char_p)
_fn("crnet_client_config_add_trust_anchors", c_int, _P, c_char_p)
_fn("crnet_client_config_add_trust_anchors_file", c_int, _P, c_char_p)
_fn("crnet_client_config_set_client_cert", c_int, _P, c_char_p, c_char_p)
_fn("crnet_client_config_set_client_cert_file", c_int, _P, c_char_p,
    c_char_p)
_fn("crnet_client_config_set_http_cache", c_int, _P, c_int, c_char_p,
    c_uint64)
_fn("crnet_client_config_set_timeout", None, _P, c_uint64)
_fn("crnet_client_config_set_connect_timeout", None, _P, c_uint64)

# Client
_fn("crnet_client_new", _P, _P)
_fn("crnet_client_free", None, _P)
_fn("crnet_client_close_idle", None, _P)
_fn("crnet_client_set_cookie", c_int, _P, c_char_p, c_char_p)
_fn("crnet_client_get_cookies", c_void_p, _P, c_char_p)
_fn("crnet_client_clear_cookies", c_int, _P)
_fn("crnet_client_export_cookies", c_void_p, _P, c_int)
_fn("crnet_client_import_cookies", c_int, _P, c_char_p, c_int,
    POINTER(c_size_t))

# Request
_fn("crnet_request_new", _P, c_char_p, c_char_p)
_fn("crnet_request_free", None, _P)
_fn("crnet_request_set_header", c_int, _P, c_char_p, c_char_p)
_fn("crnet_request_set_body", c_int, _P, c_char_p, c_size_t)
_fn("crnet_request_set_body_file", c_int, _P, c_char_p, c_uint64, c_uint64)
_fn("crnet_request_set_body_stream", c_int, _P, c_int64)
_fn("crnet_request_set_response_stream", c_int, _P, c_int)
_fn("crnet_request_set_mode", c_int, _P, c_int)
_fn("crnet_request_set_cache_mode", c_int, _P, c_int)
_fn("crnet_request_set_priority", c_int, _P, c_int)
_fn("crnet_request_set_max_redirects", c_int, _P, c_int)
_fn("crnet_request_set_timeout", c_int, _P, c_uint64)
_fn("crnet_request_set_connect_timeout", c_int, _P, c_uint64)
_fn("crnet_request_set_decompress", c_int, _P, c_int)
_fn("crnet_request_set_max_body_size", c_int, _P, c_uint64)
_fn("crnet_request_set_ws_compression", c_int, _P, c_int)
_fn("crnet_request_set_proxy", c_int, _P, c_char_p)

# Transfers
_fn("crnet_start", _P, _P, _P, c_void_p, c_void_p)
_fn("crnet_transfer_cancel", None, _P)
_fn("crnet_transfer_wait", c_int, _P, c_uint64)
_fn("crnet_transfer_done", c_int, _P)
_fn("crnet_transfer_wait_response", c_int, _P, c_uint64)
_fn("crnet_transfer_result", c_int, _P)
_fn("crnet_transfer_response", _P, _P)
_fn("crnet_transfer_take_response", _P, _P)
_fn("crnet_transfer_read", c_int, _P, c_void_p, c_size_t,
    POINTER(c_size_t), c_uint64)
_fn("crnet_transfer_write", c_int, _P, c_char_p, c_size_t,
    POINTER(c_size_t), c_uint64)
_fn("crnet_transfer_write_end", c_int, _P)
_fn("crnet_transfer_write_fd", c_int, _P)
_fn("crnet_transfer_fd", c_int, _P)
_fn("crnet_transfer_response_fd", c_int, _P)
_fn("crnet_transfer_done_fd", c_int, _P)
_fn("crnet_transfer_free", None, _P)

# Response
_fn("crnet_response_free", None, _P)
_fn("crnet_response_status", c_int, _P)
_fn("crnet_response_status_text", c_char_p, _P)
_fn("crnet_response_url", c_char_p, _P)
_fn("crnet_response_redirect_count", c_int, _P)
_fn("crnet_response_header_count", c_size_t, _P)
_fn("crnet_response_header_at", c_int, _P, c_size_t, POINTER(c_char_p),
    POINTER(c_char_p))
_fn("crnet_response_body", c_void_p, _P, POINTER(c_size_t))
_fn("crnet_response_protocol", c_char_p, _P)
_fn("crnet_response_remote_address", c_char_p, _P)
_fn("crnet_response_via_proxy", c_int, _P)
_fn("crnet_response_from_cache", c_int, _P)
_fn("crnet_response_socket_reused", c_int, _P)
_fn("crnet_response_connection_id", c_uint64, _P)
_fn("crnet_response_decoded_codings", c_char_p, _P)
_fn("crnet_response_net_error", c_int, _P)
_fn("crnet_response_error_string", c_char_p, _P)

# WebSocket
_fn("crnet_ws_connect", _P, _P, _P, c_char_p, c_void_p, c_void_p)
_fn("crnet_ws_wait_open", c_int, _P, c_uint64)
_fn("crnet_ws_send", c_int, _P, c_int, c_char_p, c_size_t)
_fn("crnet_ws_recv", c_int, _P, POINTER(c_int), POINTER(c_void_p),
    POINTER(c_size_t), c_uint64)
_fn("crnet_ws_fd", c_int, _P)
_fn("crnet_ws_open_fd", c_int, _P)
_fn("crnet_ws_closed_fd", c_int, _P)
_fn("crnet_ws_close", c_int, _P, c_int, c_char_p)
_fn("crnet_ws_ping", c_int, _P, c_char_p, c_size_t)
_fn("crnet_ws_set_idle_timeout", None, _P, c_uint64)
_fn("crnet_ws_set_auto_ping", None, _P, c_uint64)
_fn("crnet_ws_wait_closed", c_int, _P, c_uint64)
_fn("crnet_ws_get_state", c_int, _P)
_fn("crnet_ws_protocol", c_char_p, _P)
_fn("crnet_ws_extensions", c_char_p, _P)
_fn("crnet_ws_handshake_response", _P, _P)
_fn("crnet_ws_close_code", c_int, _P)
_fn("crnet_ws_close_reason", c_char_p, _P)
_fn("crnet_ws_net_error", c_int, _P)
_fn("crnet_ws_buffered_amount", c_uint64, _P)
_fn("crnet_ws_free", None, _P)

del _name


def _check_version():
    v = crnet_version().decode().split(" ", 1)[0]
    got = tuple(int(x) for x in v.split("."))
    if got < MIN_VERSION:
        raise ImportError("libcrnet %s is too old; the Python module needs "
                          "%d.%d.%d or newer" % ((v,) + MIN_VERSION))


_check_version()
