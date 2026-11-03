# SPDX-License-Identifier: MIT
"""Devicetree-to-settings-schema adapter for runtime Tap Dance records."""

import re
from types import SimpleNamespace


def _default_record(context, node, slot):
    field = f"tap-dance slot {slot}"
    output_properties = ("default-tap", "default-hold", "default-double-tap", "default-tap-hold")
    outputs = [context.default_behavior_binding(node, prop, field) for prop in output_properties]
    term_ms = context.node_value(node, "default-term-ms")
    if not any(output is not None for output in outputs):
        if term_ms is not None:
            raise ValueError(f"{field}: default-term-ms requires at least one default output")
        return [1] + [0] * 44
    if term_ms is None:
        term_ms = 200
    if type(term_ms) is not int or not 10 <= term_ms <= 500:
        raise ValueError(f"{field}: default-term-ms must be in 10..500")
    record = [1, 1, term_ms & 0xFF, (term_ms >> 8) & 0xFF]
    record.append(sum(1 << index for index, output in enumerate(outputs) if output is not None))
    for output in outputs:
        if output is None:
            record.extend([0] * 10)
            continue
        identity, param1, param2 = output
        record.extend((identity & 0xFF, identity >> 8))
        for param in (param1, param2):
            record.extend((param >> shift) & 0xFF for shift in (0, 8, 16, 24))
    return record


def contribute(context):
    nodes = [n for n in context.nodes
             if n.status == "okay" and "sekigon,dynamic-behavior-tap-dance" in set(n.compats)]
    context.validate_slot_count("tap-dance", nodes)
    maximum = context.config_values.get(
        "CONFIG_ZMK_DYNAMIC_TAP_DANCE_MAX_SLOTS",
        max((context.node_value(node, "slot-index") for node in nodes), default=-1) + 1,
    )
    if type(maximum) is not int or maximum < len(nodes):
        raise ValueError(
            "tap-dance dispatcher count exceeds "
            f"CONFIG_ZMK_DYNAMIC_TAP_DANCE_MAX_SLOTS={maximum}"
        )
    if maximum == 0:
        return []
    declared = {
        (context.node_value(n, "module-name"), context.node_value(n, "group-name"),
         context.node_value(n, "instance-name"))
        for n in context.nodes if n.status == "okay" and "sekigon,dynamic-settings-group" in set(n.compats)
    }
    groups = []
    node_by_slot = {context.node_value(node, "slot-index"): node for node in nodes}
    default_target_role = context.default_target_role()
    for slot in range(maximum):
        node = node_by_slot.get(slot)
        actual_device = node
        if node is None:
            node = SimpleNamespace(
                status="okay", compats={"sekigon,dynamic-behavior-tap-dance"},
                props={"slot-index": SimpleNamespace(val=slot),
                       "target-role": SimpleNamespace(val=default_target_role)},
                labels=[f"runtime_tap_dance{slot}"], name=f"runtime_tap_dance{slot}",
            )
            actual_device = None
        if ("tap-dance", "slot", f"slot{slot}") in declared:
            continue
        labels = getattr(node, "labels", [])
        if not labels or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", labels[0]):
            raise ValueError(f"tap-dance slot {slot}: dispatcher needs a devicetree label")
        record = _default_record(context, node, slot)
        if len(record) != 45:
            raise ValueError(f"tap-dance slot {slot}: record-default must contain exactly 45 bytes")
        groups.append(context.make_group({
            "module-name": "tap-dance", "group-name": "slot", "instance-name": f"slot{slot}",
            "label": f"Tap Dance slot {slot}", "scope": "shared", "format-version": 1,
            "slot-index": slot,
            "target-role": context.node_value(node, "target-role", "keyboard-right"),
            "actual-device": actual_device, "record-type": "tap-dance-v1", "record-max-length": 45,
            "record-on-change-context-free": actual_device is None,
            "record-default": record, "record-on-change": "dynamic_tap_dance_record_changed",
        }))
    return groups
