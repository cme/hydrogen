#!/usr/bin/env python3

import os
import sys
import json

DIR = 1
LINK = 2
FILE = 3

# Scan filesystem
def scanTreeState(path):
    if os.path.isdir(path):
        yield [DIR, path]
        for leaf in os.listdir(path):
            p = os.path.join(path, leaf)
            for item in scanTreeState(p):
                yield item
    elif os.path.islink(path):
        yield [LINK, path ]
    else:
        yield [FILE, path, os.path.getmtime(path)]

# Write the tree in JSON format
def writeTree(filename, tree):
    f = open(filename, 'w')
    json.dump(list(tree), f, indent=1)
    f.close()

def readTree(filename):
    with open(filename, "r") as f:
        if f == None:
            print("*** couldn't open " + filename)
        return json.load(f)

# Find updates
def findUpdates(a, b):
    a_map = {}
    # print("Building map")
    for o in a:
        if o[0] == FILE:
            a_map[ o[1] ] = o[2]
        else:
            a_map[ o[1] ] = True

    # print("Finding updates")
    for o in b:
        if o[1] in a_map:
            # print("Existing object " + o[1])
            if o[0] == FILE and o[2] != a_map[ o[1] ]:
                # print("Updated file")
                print(o[1])
        elif o[0] == FILE or o[0] == LINK:
            #print("New object " + o[1])
            #print(o)
            print(o[1])

def help():
    print(sys.argv[0] + " scan path stateFile")
    exit()


if len(sys.argv) < 2:
    help()

if sys.argv[1] == 'scan':
    if len(sys.argv) != 4:
        help()

    path = sys.argv[2]
    stateFile = sys.argv[3]

    tree = scanTreeState(path)
    writeTree(stateFile, tree)

elif sys.argv[1] == 'updates':
    if len(sys.argv) != 4:
        help()

    a = sys.argv[2]
    b = sys.argv[3]

    findUpdates(readTree(a), scanTreeState(b))

else:
    print("Unknown command: " + sys.argv[1])
    help()

