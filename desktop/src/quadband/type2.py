"""Tag selection and capacity using the shared guarded NTAG policy. SPDX-License-Identifier: GPL-3.0-only"""
from .common import require
from . import ndef
from .ntag import Type2NDEF, Client as Type2Client

__all__ = ['Type2NDEF', 'Type2Client', 'tag_types', 'capacity', 'check_fit']
CAPACITIES = {'classic1k': 716, 'ntag213': 142, 'ntag215': 492, 'ntag216': 868}


def tag_types():
    return {f'{name.upper()} · {size} message bytes': name for name, size in CAPACITIES.items()}


def capacity(tag):
    require(tag in CAPACITIES, 'Choose a supported bulk tag family.')
    return CAPACITIES[tag]


def check_fit(tag, records):
    size = len(ndef.encode(records))
    require(size <= capacity(tag), f'Records need {size} bytes; {tag} holds {capacity(tag)}.')
    return size
