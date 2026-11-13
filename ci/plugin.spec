Name: @PLUGIN_NAME_FEDORA@
Version: @VERSION@
Release: @RELEASE@%{?dist}
Summary: FaithfulFrame plugin for face tracking and digital pan/zoom in OBS Studio
URL: https://github.com/pakjoseph/faithfulframe
License: GPL-2.0-only AND LGPL-2.1-or-later AND BSL-1.0

Source0: %{name}-%{version}.tar.bz2
Source1: %{name}-%{version}-libvisca.tar.bz2
Source2: %{name}-%{version}-dlib-models.tar.bz2
Requires: obs-studio >= @OBS_VERSION@
BuildRequires: cmake, gcc, gcc-c++, rustup
BuildRequires: obs-studio-devel
BuildRequires: qt6-qtbase-devel qt6-qtbase-private-devel
BuildRequires: dlib-devel ffmpeg-free-devel sqlite-devel blas-devel lapack-devel
BuildRequires: flexiblas-devel
BuildRequires: libjxl-devel
# dlib-devel requires /usr/include/ffmpeg so that install ffmpeg-free-devel

%package data
Summary: Model file for %{name}
BuildArch: noarch
License: CC0-1.0 AND BSL-1.0

%description
This plugin tracks face of a person by detecting and tracking a face.

This plugin employs dlib on face detection and object tracking. The frame of
the source is periodically taken to face detection algorithm. Once a face is
found, the face is tracked. Based on the location and the size of the face
under tracking, the frame will be cropped.

%description data
Model files for @PLUGIN_NAME_FEDORA@.
The model files came from https://github.com/davisking/dlib-models/.

%prep
%autosetup -p1
%setup -T -D -a 1
%setup -T -D -a 2

%build
%{cmake} \
 -DQT_VERSION=6 \
 -DINSTALL_LICENSE_FILES:BOOL=OFF \
 -DWITH_DLIB_SUBMODULE=OFF \
 -DBUILD_SHARED_LIBS:BOOL=OFF
%{cmake_build}

%install
%{cmake_install}

mkdir -p %{buildroot}/%{_datadir}/licenses/%{name}/
mkdir -p %{buildroot}/%{_datadir}/licenses/%{name}-data/
cp LICENSE %{buildroot}/%{_datadir}/licenses/%{name}/
cp libvisca/COPYING %{buildroot}/%{_datadir}/licenses/%{name}/LICENSE-libvisca.txt
cp %{buildroot}/%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/LICENSE-dlib %{buildroot}/%{_datadir}/licenses/%{name}-data/
mv %{buildroot}/%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/LICENSE-dlib %{buildroot}/%{_datadir}/licenses/%{name}/
mv %{buildroot}/%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/LICENSE-dlib-models %{buildroot}/%{_datadir}/licenses/%{name}-data/

%files
%{_libdir}/obs-plugins/@PLUGIN_NAME@.so
%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/locale/
%{_datadir}/licenses/%{name}/*

%files data
%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/dlib_cnn_model
%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/dlib_face_landmark_model/shape_predictor_5_face_landmarks.dat
%{_datadir}/obs/obs-plugins/@PLUGIN_NAME@/dlib_hog_model
%{_datadir}/licenses/%{name}-data/*
