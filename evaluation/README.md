# TickLE Performance Evaluation

Evaluate and compare performance of middlewares.

## List of middlewares
* TickLE
* FastDDS
* CycloneDDS
* Zenoh-pico

## Directory structure

```
./ # TickLE evaluation directory
tickle/
fastdds/
cyclonedds/
zenoh-pico/
```

## Environment

### TickLE
- branch: ([]())

### FastDDS
- branch: master([dd66ef2aff7230c7d39862dc7384402b88156d6f](https://github.com/eProsima/Fast-DDS/tree/dd66ef2aff7230c7d39862dc7384402b88156d6f))
- Installation guide: [Linux installation from source](https://fast-dds.docs.eprosima.com/en/latest/installation/sources/sources_linux.html)
#### Installation guide summary
##### Dependencies
```bash
sudo apt install cmake g++ python3-pip wget git
sudo apt install libasio-dev libtinyxml2-dev
sudo apt install libssl-dev
sudo apt install libp11-dev
sudo apt install softhsm2
sudo usermod -a -G softhsm $USER # re-login to take effect
sudo apt install libengine-pkcs11-openssl
p11-kit list-modules # Check if p11kit uses SoftHSM
openssl engine pkcs11 -t # Check if OpenSSL uses pkcs11 engine
```

##### Colcon
```bash
pip3 install -U colcon-common-extensions vcstool # python virtual environment may be needed depending on python version
cd fastdds/lib
wget https://raw.githubusercontent.com/eProsima/Fast-DDS/master/fastdds.repos
mkdir src
vcs import src < fastdds.repos
colcon build --packages-up-to fastdds
```

##### ENV
```bash
source install/setup.$(basename ${SHELL})
```

##### FastDDS Gen (Typesupport)
```bash
sudo apt install openjdk-17-jdk
git clone --recursive https://github.com/eProsima/Fast-DDS-Gen.git src/fastddsgen
cd src/fastddsgen
./gradlew assemble

# If system has jdk already
export JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 # directory name may be different
./gradlew assemble
```

### CycloneDDS
- commit or branch

### Zenoh-pico
- commit or branch

## Build & Run

### TickLE

### FastDDS
#### Build
```bash
source fastdds/lib/install/setup.$(basename ${SHELL})
cd fastdds/rtt
cmake -S . -B src
```
#### Run
Run ping & pong application in separate terminal.
```bash
bash run_ping.sh [-i interval_ms] [-c count]
bash run_pong.sh
```

### CycloneDDS

### Zenoh-pico

- build command or script
- run command or script
