#!/usr/bin/env python3
"""One-shot CLI for the Medula SOAP engine.

    MEDULA_ECZANE_KODU=... MEDULA_PAROLA=... python3 query.py [dd.MM.yyyy]
    ... python3 query.py --ilac aktif [dd.MM.yyyy]

Defaults to the daily stock query; ``--ilac`` queries the reimbursable drug
list (note: returns 9108 "not authorized" for pharmacy credentials today).
"""

from __future__ import annotations

import argparse
import json
import sys

from engine import ConfigError, DrugListClient, SoapError, StockClient, load_config

COLOURS = ("yesil", "kirmizi", "turuncu", "mor")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tarih", nargs="?", help="dd.MM.yyyy (defaults to today)")
    parser.add_argument("--config", help="path to config.json")
    parser.add_argument(
        "--ilac",
        nargs="?",
        const="aktif",
        help="query drug list instead: aktiv|pasif|normal|yesil|kirmizi|turuncu|mor",
    )
    args = parser.parse_args()

    try:
        config = load_config(args.config)
    except ConfigError as exc:
        print(exc, file=sys.stderr)
        return 2

    try:
        if args.ilac:
            client = DrugListClient(config)
            kind = args.ilac.lower()
            if kind == "aktif":
                result = client.active(args.tarih)
            elif kind == "pasif":
                result = client.passive(args.tarih)
            elif kind == "normal":
                result = client.normal(args.tarih)
            elif kind in COLOURS:
                result = client.by_colour(kind, args.tarih)
            else:
                print(f"unknown --ilac kind: {args.ilac}", file=sys.stderr)
                return 2
        else:
            result = StockClient(config).daily_stock(args.tarih)
    except SoapError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
