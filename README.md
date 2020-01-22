# DEMU （DPDK-based Network Emulator）

Copyright (c) 2017-2019 National Institute of Advanced Industrial Science and Technology (AIST), Japan

## Overview
DEMU is a software-based network emulator implemented as a DPDK application. Conventional software-based emulator such as NetEm and dummynet have problems on the throughput of packet forwarding and the accuracy of emulation. DEMU addresses this problem by using a high-performance user-land network framework, i.e., DPDK, and it can emulate latencies on the order of 10ms for short-packet traffic at the 10GbE line rate with high accuracy. 

DEMU is released under the BSD-3-Clause license.

### Features

- Accurate delay emulation in microseconds
- Accurate packet loss emulation
  - Random loss
  - Burst loss based on the Gilbert-Elliott model
- Packet duplication
- Bandwidth limitation


## Getting Started

### Supported Environment

- Ubuntu 18.04
- Linux kernel 4.15.0
- DPDK 17.11.3

### Preparation

#### hugepage

DEMU requires a large buffer space for emulating a large bandwidth-delay product (BDP) network. DEMU allocates hugepages for such a buffer space. For example, when you assign 12,000 2MB pages (i.e., 24GB) on a CPU socket (node0), you should set up as follows:

```shell
echo 12000 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
```

#### isolcpus

We recommend to add the linux kernel command line parameter `isolcpus` to exclusively allocate CPU cores for DEMU and the Linux kernel. It allows to perform the accurate emulation without the intervention of the Linux kernel. For example, you should add the following line in /etc/default/grub:

```shell
GRUB_CMDLINE_LINUX="isolcpus=2-7"
```

### Building

#### DPDK

DEMU relies on DPDK, and you should build DPDK from the source code. You can find the detail instruction in the getting started guide at the DPDK web page.

```shell
$ git clone git@github.com:ryousei/demu.git
$ cd demu
$ git submodule update --init
$ cd dpdk
$ make install T=x86_64-native-linuxapp-gcc
$ export RTE_SDK=$PWD
```

#### DEMU

```shell
$ cd ..
$ make
```

### Usage

This figure shows an example configuration, where each box represents PC. DEMU is running on the middle machine which has two network interface cards (e.g., enp1s0f0, enp1s0f1).

```
+--------+     +------------+     +--------+
|        |     |NIC      NIC|     |        |
| sender +-----+#0  DEMU  #1+-----+receiver|
|        |     |            |     |        |
+--------+     +------------+     +--------+
               NIC #0: enp1s0f0
               NIC #1: enp1s0f1
```

First, you should set up device drivers on DEMU machine as follows:

```shell
$ sudo modprobe uio_pci_generic
$ sudo ifconfig enp1s0f0 down
$ sudo ifconfig enp1s0f1 down
$ sudo $RTE_SDK/usertools/dpdk-devbind.py --bind=uio_pci_generic enp1s0f0
$ sudo $RTE_SDK/usertools/dpdk-devbind.py --bind=uio_pci_generic enp1s0f1
```

Next, you issues demu command with options as follows:

```shell
$ sudo ./build/demu -c fc -n 4 -- -p 3 -d <delay time [us]> -r <packet loss rate [%]>
```

Ctrl+c terminates the DEMU process.



For packet loss based on Gilbert-Elliott model,

```shell
$ sudo ./build/demu -c fc -n 4 -- -p 3 -d <delay time [us]> \
                                  -r <probability from Good state to Bad state [%]> \
                                  -g <probability from Bad state to Good state [%]>
```

For bandwidth limtation, you can specify the target rate as `-s <speed>[K|M|G]`. For example, `1G` means 1 Gbps. Note: DEMU assigns one extra core for a timer thread. Therefore you have to change the `--coremap (-c)` option.

```shell
$ sudo ./build/demu -c 1fc -n 4 -- -p 3 -s <speed[K/M/G]>
```

Finally, you restore the normal Linux network configuration as follows:

```shell
$ sudo $RTE_SDK/usertools/dpdk-devbind.py --bind=ixgbe 0000:01:00.0
$ sudo $RTE_SDK/usertools/dpdk-devbind.py --bind=ixgbe 0000:01:00.1
```

Note: PCI device ID (e.g., 0000:01:00.0) depends on the hardware configuration.



## Known Issues

- **Maximum number of queuing packet**: It is possible to queue up to 4M packets in the buffer. If you want to emulate a large BDP network such as 10GbE with 100ms of latency and transfer short packets over the network, you should enable the macro `SHORT_PACKET` and build the DEMU again. It is only for testing short packet (less than 1000B), so you don't enable this macro for normal emulation situations. 
- **Fixed port ID**: DEMU assumes a machine has two network interfaces (i.e., ports). Packets incomming from the port ID 0 are forwarded to the port ID 1, and vice versa. This pairing is fixed.
- **Asymmetric delay setting**: DEMU only support one-way delay emulation.



## Publications

- Shuhei Aketa, Takahiro Hirofuchi, Ryousei Takano, "DEMU: A DPDK-based Network Latency Emulator," The 23rd IEEE International Symposium on Local and Metropolitan Area Networks, pp.1-6, June 2017. [[IEEE Explore](https://ieeexplore.ieee.org/document/7972145)]
- Kanon Sasaki, Takahiro Hirofuchi, Saneyasu Yamaguchi, Ryousei Takano, "An Accurate Packet Loss Emulation on a DPDK-based Network Emulator," The 15th Asian Internet Engineering Conference, pp.1-8, August 2019. [[ACM DL](https://dl.acm.org/citation.cfm?id=3343635)]
- Chayapon Puakalong, Ryousei Takano, Vasaka Visoottiviseth, Assadarat Khurat1, Wudichart Sawangphol, "A Network Bandwidth Limitation with the DEMU Network Emulator," The 10th IEEE Symposium on Computer Applications & Industrial Electronics, April 2020. (To appear)


## Contributors

- Shuhei Aketa, Ritsumeikan University
- Kanon Sasaki, Kougakuin University
- Chayapon Puakalong, Mahidol University
- Takahiro Hirofuchi, AIST
- Ryousei Takano, AIST

