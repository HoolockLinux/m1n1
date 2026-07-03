# SPDX-License-Identifier: MIT
from ...utils import *

# System endpoints
def msg_handler(message, regtype=None):
    def f(x):
        x.is_message = True
        x.message = message
        x.regtype = regtype
        return x

    return f

class AKFTimeout(Exception):
    pass

class AKFBaseEndpoint:
    BASE_MESSAGE = Register64
    SHORT = None

    def __init__(self, akf, epnum, name=None):
        self.akf = akf
        self.epnum = epnum
        self.name = name or self.SHORT or f"{type(self).__name__}@{epnum:#x}"

        self.msghandler = {}
        self.msgtypes = {}
        for name in dir(self):
            i = getattr(self, name)
            if not callable(i):
                continue
            if not getattr(i, "is_message", False):
                continue
            self.msghandler[i.message] = i
            self.msgtypes[i.message] = i.regtype if i.regtype else self.BASE_MESSAGE

    def handle_msg(self, msg):
        msg = self.BASE_MESSAGE(msg)
        handler = self.msghandler.get(msg.TYPE, None)
        regtype = self.msgtypes.get(msg.TYPE, self.BASE_MESSAGE)

        if handler is None:
            return False
        return handler(regtype(msg.value))

    def send(self, msg):
        msg.EP = self.epnum
        self.akf.send(msg)

    def start(self):
        pass

    def stop(self):
        pass

    def log(self, msg):
        print(f"[{self.name}] {msg}")
