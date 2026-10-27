# CarPlay lanes on the cluster

Source: `0x5204 LaneGuidanceInformation`. Code: `native/src/common/route.c` → UDP PhoneLink →
`hmi/src/mst2/clusternav/Lanes.java` → `Datapool.write` → the stock
`ASLNavBAPAPI.updateBapLaneGuidance`. This adds to the native BAP arrows and is independent of the
cluster map video.

## The phone's side

The iAP2 schema follows the public description of the protocol, including a published
three-lane sample. It describes the protocol; it does not prove what a given app sends to P0480T.

- `0x5204/1` is the instruction index; each nested `0x5204/2` is one lane. Lane fields: 0 position
  u16, 1 status u8, 2 (repeatable) angle i16, 3 a separate highlighted angle i16, which need not
  be among the fields 2.
- Status 0 not recommended, 1 allowed, 2 preferred. Angles are degrees, negative to the left;
  ±1000 and values outside ±180 are no direction.
- Position 0 is taken as the leftmost lane, as in the public samples; the car with an iPhone
  confirms it.
- `0x5201/16` selects the instruction, `0x5201/18` allows showing it. The lane cache is separate
  from the maneuver cache: an empty maneuver list does not hide pre-positioning lanes.
- Six instructions are kept, up to 16 lanes and 16 plain angles a lane. Beyond that the whole
  instruction is hidden: a road layout is never cut short without notice.

## PhoneLink

Each datagram is a full UTF-8 guidance snapshot of at most 4096 bytes. Extra lines:

```text
lanes=3
lane0=0,0,1000,-90
lane1=1,1,1000,0
lane2=2,2,90,0
```

`laneN=position,status,highlighted_angle,plain_angle,...`; 1000 as the highlighted angle means
none. `lanes=0` hides the lanes, and so does a full snapshot without lane lines, so older senders
and replays stay compatible. Java sorts the lanes by position. An incomplete or broken set, a
repeated position or line, the unknown position 65535 and a set without any recognisable
direction hide the lanes.

## Stock HMI and BAP

Evidence: `javap -c -p -constants` (JDK 8) on the P0480T MIBHMI.jar (SHA-256
`f5013eb9864a5aa5748ec60651ab724a75b73f8a51d5421be5fb034974d6c2cd`); the class and method names
below repeat the check.

- `asl.internal.navbap.api.impl.ASLNavBAPAPIImpl.updateBapLaneGuidance` takes
  `INavLaneGuidanceDataNavBap[]` and a boolean. False or null clears the list. True writes the
  fields to ASL list 742: 0 direction, 1 side directions, 2 type, 3/4 markings, 5 description,
  6 recommendation. The array order is kept.
- `bap.mqbab2.navsd.functions.LaneGuidance.getFunctionId` returns 24 (`0x18`).
  `computeFullLaneGuidanceList` reads these fields unchanged, keeps up to six lanes and marks
  hidden edges with types 23/24. It shows lanes at route guidance state 1 with the stock lane
  guidance setting on.
- `NavigationASLDataAdapter` reads the state from property 732 and the setting from 731 (default
  true). The mod does not change the user's setting.
- `bap.mqbab2.generated.navsd.serializer.LaneGuidance_LaneGuidance`: recommendation 0/1/2 =
  NOT_RECOMMENDED/RECOMMENDED/BEST; type 0 = UNSPECIFIC; description 15 =
  NOT_SUPPORTED_NO_DESCRIPTION_AVAILABLE; marking 0 = no lines. The mod does not invent a road
  type, merging lanes or solid/dashed lines.
- Directions use the 16-sector geometry of the maneuvers' BAP side streets: 0 ahead, 64 left,
  128 back, 192 right; 255 is no direction, `LANE_INVALID_DIRECTION` in the stock `LaneGuidance`.

The highlighted angle becomes the main direction, the others stay as the lane's side directions.
Without a highlight, a lane with one distinct direction keeps its recommendation; a lane with
several directions and no reliable highlight gets none, so that no arbitrarily chosen exit is lit,
while its directions stay visible.

End of route, arrival, hiding by the phone, an unknown current index, 120 s without a status and
switching the mod off clear the lanes. The stock API is called on the ASL thread.

## Checks

```sh
sh native/check.sh
MIBHMI=<MIBHMI.jar> sh hmi/check.sh
```

The native test writes a snapshot from the real C decoder to `native/build/check/lanes-message.txt`;
the HMI test reads it with the same `PhoneLink.parse` the unit runs and checks the NavBAP
interface: main and side directions, recommendations, order, hiding, unknown values and broken
sets. `replays/lanes.txt` is a synthetic scenario to replay.

### Against the cluster's own firmware

In mQbEMU, with the unmodified head-unit and cluster firmware and `replays/lanes.txt` as the
phone, the head unit answered every lane request of the cluster (BAP NavSD 0x18) as expected and
the cluster drew its own lane symbols: a preferred right lane keeping its straight side exit, a
preferred middle lane, the lane row hidden while the main arrow stays, two recommended left lanes
with an unknown direction left unlit, and an empty list at the end of the route. The mod adds no
graphics of its own. Real `0x5204` messages, and how an app keeps the indices and the present flag
in step, are checked in the car with an iPhone.
