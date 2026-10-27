# CarPlay maneuvers on the cluster

`hmi/src/mst2/clusternav/Maneuvers.java` turns a CarPlay route-guidance maneuver into the
cluster's BAP ManeuverDescriptor; `hmi/check.sh` checks all 54 types for right- and left-hand traffic on the host.

**BAP direction** (byte, counter-clockwise): 0 ahead, 32 slight left, 64 left, 96 sharp left,
128 back, 160 sharp right, 192 right, 224 slight right — 64/192/224/32 verified on the cluster firmware in mQbEMU
cluster (Continental AID 10" `5E0920790A`). iAP2 angles are degrees, negative = left; ±1000 or
an absent field means unknown.

| iAP2 type | Meaning | BAP main / direction |
|---|---|---|
| 0, 5, 15, 17 | no turn, follow road, ferry | 11 FOLLOW_STREET / 0 |
| 1, 2, 3 | left, right, straight | 13 TURN / 64, 192, 0 |
| 4, 18, 26 | U-turn family | 25 UTURN / 64 or 192 by the angle's sign, else by traffic side (right-hand traffic → 64) |
| 6 | enter roundabout | 13 / 192 (left-hand traffic 64) |
| 7 | exit roundabout | 26 / 192 (left-hand traffic 27 / 64) |
| 8, 9 | off / on ramp | 13 / 32 or 224 by sign, else by traffic side |
| 10, 12, 27 | arrive | 3 ARRIVED / 0 |
| 11, 16, 51 | start route, leave ferry, change highway | 13 / angle: ≤22° ahead, ≤67° slight, ≤112° normal, else sharp |
| 13, 49, 22 | keep left, slight left, highway off-ramp left | 13 / 32 |
| 14, 50, 23 | keep right, slight right, highway off-ramp right | 13 / 224 |
| 19 | U-turn at roundabout | 21 / 128 (left-hand traffic 22) |
| 20, 21 | turn at end of road | 13 / by angle on the type's side, else 64 / 192 |
| 24, 25 | arrive left / right | 3 / 64, 192 |
| 28…46 | roundabout exit 1…19 | 21 (left-hand traffic 22) / exit angle in 22.5° steps; without an angle (or 0): exit 1 right, 2 ahead, 3 left, later back (mirrored for left-hand traffic) |
| 47, 48 | sharp left / right | 13 / 96, 160 |
| 52, 53 | change highway left / right | 12 CHANGE_LANE / 64, 192 |
| other | — | 1 NO_INFO |

Side streets: one direction byte per junction road (22.5° steps), the exit road and unknown
angles left out, at most 17.

Sources: the iAP2 route-guidance maneuver types and their CarPlay meaning as described publicly
for the protocol; the BAP encoding from this head unit's HMI and the cluster firmware, both
studied in mQbEMU. No third-party code is used.
