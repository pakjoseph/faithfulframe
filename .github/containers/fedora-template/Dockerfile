FROM fedora:%releasever%

RUN dnf install -y rpm-build python3-dnf-plugins-core && dnf clean all
RUN dnf copr enable -y kamae/obs-studio-plugins && dnf clean all
RUN dnf install -y --setopt=install_weak_deps=false obs-studio obs-studio-devel cmake gcc gcc-c++ dlib dlib-devel rustup && dnf clean all
RUN dnf install -y qt6-qtbase-devel qt6-qtbase-private-devel && dnf clean all

RUN useradd -s /bin/bash -m rpm
RUN echo >> /etc/sudoers
RUN echo "rpm ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

USER rpm
WORKDIR /home/rpm
ENV CARGO_HOME=/home/rpm/.cargo
ENV RUSTUP_HOME=/home/rpm/.rustup
ENV PATH=/home/rpm/.cargo/bin:${PATH}
RUN rustup-init -y --no-modify-path --profile minimal --default-toolchain 1.98.0
