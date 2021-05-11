FROM amazonlinux:2

WORKDIR /home/ec2-user

# The build needs a package from the EPEL repo so that needs to be enabled.
# https://www.tecmint.com/install-epel-repository-on-centos/
RUN yum install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm

# Update and install needed build packages
RUN yum -y update
RUN yum -y group install "development tools"
RUN yum -y install \
      git cmake clang python swig uuid-devel libicu-devel libedit-devel \
      libxml2-devel sqlite-devel ncurses-devel pkgconfig python-devel \
      python-pkgconfig libbsd-devel libuuid-devel pexpect curl-devel \
      tzdata rsync wget which python-six ninja-build \
      python3 python3-devel
RUN ln -s /usr/bin/ninja-build /usr/bin/ninja

# Install sccache
RUN mkdir /opt/sccache && cd /opt/sccache && \
    wget -O - "https://github.com/mozilla/sccache/releases/download/0.2.13/sccache-0.2.13-x86_64-unknown-linux-musl.tar.gz" | \
      tar xz --strip-components 1 && \
    ln -sf /opt/sccache/sccache /usr/local/bin

# Grab an updated version of cmake
RUN wget https://cmake.org/files/v3.16/cmake-3.16.5-Linux-x86_64.tar.gz \
      && tar xvzf cmake-3.16.5-Linux-x86_64.tar.gz \
      && rm cmake-3.16.5-Linux-x86_64.tar.gz

# Add updated version of cmake to path
ENV PATH="/home/ec2-user/cmake-3.16.5-Linux-x86_64/bin/:${PATH}"


# Bootstrap the swift source and do a full checkout
ARG SWIFT_GIT_SHA
ARG SWIFT_CHECKOUT_SCHEME=wasm

RUN git clone https://github.com/swiftwasm/swift.git
WORKDIR /home/ec2-user/swift
RUN git fetch origin $SWIFT_GIT_SHA
RUN git checkout $SWIFT_GIT_SHA
RUN ./utils/update-checkout --clone --scheme $SWIFT_CHECKOUT_SCHEME --skip-repository swift
RUN ./utils/webassembly/install-build-sdk.sh

RUN ./utils/webassembly/build-toolchain.sh
