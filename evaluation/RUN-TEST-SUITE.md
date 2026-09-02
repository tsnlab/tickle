# How to run test suite
## Install dependencies
#### Ubuntu 24.04 or higher
```bash
sudo apt install python3-numpy
sudo apt install python3-matplotlib
```
#### Other environment
[NumPy Installation](https://numpy.org/install/)

[Matplotlib Installation](https://matplotlib.org/stable/install/index.html)

## Clone repository
```bash
cd <workspace>
git clone https://github.com/tsnlab/tickle.git -b middlewares_evaluation_test tickle_test
cd tickle_test/evaluation
```

## Configure test parameters (Optional)
Open `test_and_draw.sh`
* `CSV_PATH`: results will be stored here
* `RMW_LIST`: list of middlewares with ROS 2
* `MW_LIST`: list of middlewares without ROS 2 
* `INTERVAL_LIST`: list of intervals which are X-axis elements of RTT graph
* `PAYLOAD_SIZE_LIST`: not supported
* `NUM_MESSAGES`: number of ping messages per one test case

## Copy SSH public key
```bash
ssh-copy-id tsnlab@192.168.1.160
Type password
ssh-copy-id tsnlab@192.168.1.161
Type password
```

## Run test
```bash
bash test_and_draw.sh
```
Initial log
```bash
<CSV_PATH>/YYMMDD_HHMMSS does not exist; will be created
checking node connection
master node tsnlab@192.168.1.160 normal
slave node tsnlab@192.168.1.161 normal
```

## Open result
```bash
# default CSV_PATH
xdg-open $HOME/ros2/data
```
Name of result folder is `YYMMDD_HHMMSS` so that result folders do not overlap.

## Change TickLE version (Not supported yet)
Run following commands for each RPi's
```bash
ssh tsnlab@192.168.1.160
# ssh tsnlab@192.168.1.161 
cd tickle
git fetch
git checkout <target-branch>
make clean
make
git checkout middlewares_evaluation_test
cd evaluation/tickle/rtt
make
```
