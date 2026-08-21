#!/usr/bin/env python3
"""Build a play session that enters a password and starts that course.

Every course past the first is behind the password screen, so validating
them means typing.  This generates the input recording and the key
schedule together, deriving the timing from the password's length so a
ten-letter password does not have its Return pressed while it is still
being typed.

  python3 tools/course_session.py FOG "PEA SOUP" build/fog
    -> build/fog.rec, build/fog.keys
"""
import sys

RAW = {'A':0x20,'B':0x35,'C':0x33,'D':0x22,'E':0x12,'F':0x23,'G':0x24,
       'H':0x25,'I':0x17,'J':0x26,'K':0x27,'L':0x28,'M':0x37,'N':0x36,
       'O':0x18,'P':0x19,'Q':0x10,'R':0x13,'S':0x21,'T':0x14,'U':0x16,
       'V':0x34,'W':0x11,'X':0x32,'Y':0x15,'Z':0x31,' ':0x40}
BITS = {'up':1,'down':2,'left':4,'right':8,'fire':16}
KEY_GAP = 40          # frames between keystrokes
HOLD    = 6           # frames a direction/fire is held

def build(name, password, out, frames=None):
    presses = [(2200,'fire'),                       # title -> options
               (2700,'up'),(2760,'up'),(2820,'up'),  # GAME -> PASSWORD
               (2900,'fire')]                        # open the field
    f = 3000
    keys = []
    for ch in password.upper():
        if ch not in RAW:
            raise SystemExit("no raw code for %r" % ch)
        keys.append((f, RAW[ch])); f += KEY_GAP
    ret = f + KEY_GAP
    keys.append((ret, 0x44))                         # submit
    d = ret + 140
    presses += [(d,'down'),(d+60,'down'),(d+120,'down'),   # back to GAME
                (d+200,'fire'),(d+400,'fire'),(d+600,'fire')]
    total = frames or (d + 3000)
    buf = bytearray(total)
    for at, what in presses:
        for k in range(at, min(total, at+HOLD)):
            buf[k] |= BITS[what]
    open(out+'.rec','wb').write(buf)
    open(out+'.keys','w').write(''.join("%d %02x\n" % k for k in keys))
    return total, ret

if __name__ == '__main__':
    name, pw, out = sys.argv[1], sys.argv[2], sys.argv[3]
    total, ret = build(name, pw, out)
    print("%-10s %-12s %s.rec (%d frames), return at %d" % (name, pw, out, total, ret))
