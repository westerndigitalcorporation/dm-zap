# dm-zap

The dm-zap device mapper target turns a set of
sequential write required zones into conventional zones, thus allowing random
writes to a disk only exposing sequential write required zones.

The device mapper has been specifically designed for SSDs, where staging
writes on media before writing to sequential zones is not an option due 
due to the media wear-out implications.

This is useful for applications and filesystems requiring a set of conventional
zones for in-place updates of i.e. metadata.

dm-zap target devices are formatted and checked using the dmzap utility
available at: TBD

Initial Authors: Hans Holmberg, Dennis Maisenbacher, Reza Salkhordeh, and Nick Tehrany

## Compile

Enable `Device Drivers->Multiple devices driver support (RAID and LVM)->Conventional zone device target support` using `make menuconfig` and then compile the kernel normally.

## Usage

A zoned block device must be available (e.g.
[setup](https://zonedstorage.io/docs/getting-started/nullblk) a `null_blk`
device with the name "/dev/nullb1").


In order to create a dm-zap target some parameters need to be selected.
A parameter selection might look like this: 
```
device="/dev/nullb1"

# Number of conventional zones of the given device
conv="0"

# Overprovisioning rate (in %)
op_rate="30"

# Class 0 threshold of Fast Cost-Benefit (affects dm-zap GC only when
# victim_selection_method="2" is set)
class_0_cap="125"

# Class 0 optimal of Fast Cost-Benefit (affects dm-zap GC only when
# victim_selection_method="2" is set)
class_0_optimal="25"

# Pick a GC victim selection method (0: Greedy, 1: Cost-Benefit,
# 2: Fast Cost-Benefit, 3: Approximative Cost-Benefit, 4: Constant Greedy,
# 5: Constant Cost-Benefit, 6: FeGC, 7: FaGC+)
victim_selection_method="0"

# Limit of free zones (in %) when the GC should start reclaiming space 
reclaim_limit="10"

# q limit of Approximative Cost-Benefit victim selection (affects dm-zap GC
# only when victim_selection_method="3" is set)
q_cap="30" 
```
Further information about the victim selection methods:
- [A Flash-Memory Based File System](https://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.92.2279&rep=rep1&type=pdf)
- [Constant Time Garbage Collection in SSDs](https://ieeexplore.ieee.org/document/9605386)
- [ENVy: A Non-Volatile, Main Memory Storage System](https://dl.acm.org/doi/10.1145/195470.195506)
- [Time-efficient Garbage Collection in SSDs](https://arxiv.org/abs/1807.09313)


Finally the dm-zap device mapper target can be created:
```
echo "0 `blockdev --getsize ${device}` zap ${device} ${conv} ${op_rate} ${class_0_cap} ${class_0_optimal} ${victim_selection_method} ${reclaim_limit} ${q_cap}" | sudo dmsetup create dmzap-test-target
```

## Community

For help or questions about dm-zap usage (e.g. "how do I do X?") see below, on join us on [Matrix](https://app.element.io/#/room/#zonedstorage-general:matrix.org), or on [Slack](https://join.slack.com/t/zonedstorage/shared_invite/zt-uyfut5xe-nKajp9YRnEWqiD4X6RkTFw).

To report a bug, file a documentation issue, or submit a feature request, please open a GitHub issue.

## Design

dm-zap is designed to be a low-overhead(disk/host mem usage) adapter to enable
file systems and applications to store a limited amount of data in an area that
allows random writes.

### Disk format

```
Zones 0..1:    Reserved for superblock storage and for
               persisting mapping data for data in open zones

Zones 2..R-1:  Zones used for storing random writes

Zones R..N-1:  Zones used for storing sequential writes
               Writes to this area are just remapped and
               passed through to the backing media.
```
N = Number on zones reported by backing media

Example:

```
Zone      Type    Contents                   Comment
                   _______________________
Zone 0    Meta    |HMMMMMMMMMMMMMMMMMMMMMM|  Full ZAP_META zone
                  |MMMMMMMMMMMMMMMMMMMMMMM|
                   -----------------------
Zone 1    Meta    |HMMMMMMMM              |  Active ZAP_META zone
                  |                       |
                   -----------------------
Zone 2    Rand    |HDDDDDDDDDDDDDDDDDDDDDD|  Full ZAP_RAND zone
                  |DDDDDDDDDDDDDDDDMMMMMMF|
                   -----------------------
Zone 3    Rand    |HDDDDDDDDDDDDDDDDDDDDDD|  Active ZAP_RAND zone
                  |DDDDDDDDD              |
                   -----------------------
Zone ..   Rand    |.......................|  Full ZAP_RAND zone
                  |.......................|
                   -----------------------
Zone R-1  Rand    |HDDDDDDDDDDDDDDDDDDDDDD|  Full ZAP_RAND zone
                  |DDDDDDDDDDDDDDDDDDMMMMF|
                   -----------------------
Zone R    Seq     |DDDDDDDDDDDDDDDDDDDDDDD|  Full user sequential data zone
                  |DDDDDDDDDDDDDDDDDDDDDDD|
                   -----------------------
Zone R+1  Seq     |DDDDDDDDDDDDDDDDDDDDDDD|  Open sequential data zone
                  |DDDD                   |
                   -----------------------
Zone ..   Seq     |.......................|  Sequential data zone
                  |.......................|
                   -----------------------
Zone N-1  Seq     |DDDDDDDDDDDDDDDDDDDDDDD|  Last zone of the disk
                  |DDDD                   |
                   -----------------------
```
Legend: M = Mapping data, H = Zone header, F = Zone footer D = User data
        (4k blocks)

### Metadata

A zone header is stored at offset 0 of all zones that are managed by dm-zap.
A zone footer is stored for all random zones.

The header and footers are stored as one dm-zap block (4k).

Header format (512 bytes)

```
Field           Size(bytes)   Offset  Comment

MAGIC           4               0     {'z','a','p','0'}
VERSION         4               4
SEQUENCE_NR     4               8     Increases monotonically for each
                                      written header
TYPE            4              12     Types: ZAP_META = 0 / ZAP_RAND = 1
RESERVED        492            16     Reserved for future use. Filled with 0xFF
CRC             4             504     CRC of all above fields
```

Footer format (512 bytes)

```
Field           Size(bytes)  Offset   Comment

SEQUENCE_NR     4               0     Copy of SEQUENCE_NR in the zone header
MAPPING_START   4               4     Start of zone mapping data
                                      relative to zone start adress
RESERVED        500             8     Reserved for future use. Padded with 0xFF
CRC             4             508     CRC of zone mapping data + above fields
```

The logical-to-device mapping for the random zones are stored at the end of each
random zone. To prevent dataloss because of crashes/powerloss, the mapping data
for the active random zone is stored in the metadata blocks until the random
zone is closed to gurantee that data can be read from disk after a
sync / REQ_PREFLUSH request.

Mapping entry format (8 bytes)

```
Field           Size(bytes)  Offset   Comment
START           4               0     Logical start block
LENGTH          4               4     Length of write, in 4k blocks
ADDRESS         4               8     Backing block adress
```

### User view

A number of zones is exposed to the user by dm-zap, first a number(C) of
conventional zones that are managed by dm-zap, followed by a number of sequential
write required zones that dm-zap only remaps and passes on to the backing media.

Note that the number of backing zones (R) for the conventional zones is bigger than C.
In addition to user data, the backing zones needs to store mapping metadata and reserve
space for doing garbage collection.

The zone size is the typical zone size reported by the backing media.

```
Zone        Type
Zone 0      BLK_ZONE_TYPE_CONVENTIONAL
Zone 1      BLK_ZONE_TYPE_CONVENTIONAL
Zone ..     BLK_ZONE_TYPE_CONVENTIONAL
Zone C      BLK_ZONE_TYPE_CONVENTIONAL
Zone C+1    BLK_ZONE_TYPE_SEQWRITE_REQ
Zone ..     BLK_ZONE_TYPE_SEQWRITE_REQ
Zone N-1    BLK_ZONE_TYPE_SEQWRITE_REQ
```
