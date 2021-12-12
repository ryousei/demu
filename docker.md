## How to use DEMU for inter-container network emulation

docker0 is a bridge device created by Docker. In order to emulate inter-container network, we use DEMU instead of docker0.
```
<original docker bridge networking>

container          host           container
+--------+     +------------+     +--------+
| sender |     |   docker0  |     |receiver|
|    eth0+-----+veth0  veth1+-----+eth0    |
|        |     |            |     |        |
+--------+     +------------+     +--------+
```

```
<DEMU>

container          host           container
+--------+     +------------+     +--------+
| sender |     |    DEMU    |     |receiver|
|    eth0+-----+veth0  veth1+-----+eth0    |
|        |     |            |     |        |
+--------+     +------------+     +--------+
```

Here we show the step-by-step instructions.
First, you create a network called demu for this experiment.

```shell
sudo docker network create --subnet 192.168.0.0/24 demu
```

And then two docker containers are launched as follows:

```shell
sudo docker run --network demu -it ubuntu /bin/bash 
```

```shell
sudo docker run --network demu -it ubuntu /bin/bash 
```

In the next step, you remove the host-side veth interfaces from the bridge device.
```
$ sudo brctl show
bridge name	bridge id		STP enabled	interfaces
br-0dc323a30514		8000.0242503cb2f2	no		veth0847ac1
							veth28522ae
docker0		8000.024285fc5ec9	no

$ sudo brctl delif br-0dc323a30514 veth0847ac1 
$ sudo brctl delif br-0dc323a30514 veth28522ae

$ sudo brctl show
bridge name	bridge id		STP enabled	interfaces
br-0dc323a30514		8000.0242503cb2f2	no
docker0		8000.024285fc5ec9	no
```

Finally you run demu, and then all packets from containers go through demu.
```shell
sudo ./build/demu --vdev=net_af_packet0,iface=veth0847ac1 --vdev=net_af_packet1,iface=veth28522ae -c fc -n 4 -- -p 3 -d 1000
```

Enjoy!

