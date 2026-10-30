Name:           vkintox
Version:        0.2.0
Release:        1%{?dist}
Summary:        Vulkan post-processing layer with overlay UI

License:        Zlib
URL:            https://github.com/VKIntox/VKIntox
Source0:        %{name}-%{version}.tar.xz

BuildRequires:  meson
BuildRequires:  clang
BuildRequires:  pkgconf-pkg-config
BuildRequires:  vulkan-headers
BuildRequires:  libX11-devel
BuildRequires:  libXi-devel
BuildRequires:  libXtst-devel
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  libxkbcommon-devel
BuildRequires:  glib2-devel

%if 0%{?suse_version}
BuildRequires:  ninja
BuildRequires:  glslang-devel
BuildRequires:  spirv-headers
%else
BuildRequires:  ninja-build
BuildRequires:  glslang
BuildRequires:  spirv-headers-devel
%endif

%global debug_package %{nil}

%description
VKIntox is a Vulkan post-processing layer with advanced depth buffer resolve
and an in-game Material 3 overlay, for Wayland and X11.

%prep
%autosetup -n %{name}-%{version}

%build
%meson \
    --buildtype=release \
    -Dappend_libdir_vkintox=false
%meson_build

%install
%meson_install

%files
%{_libdir}/libvkintox.so
%{_bindir}/vkintox-run
%{_bindir}/vkintox-shader-check
%{_datadir}/vulkan/implicit_layer.d/VKIntox.json
%license %{_datadir}/licenses/vkintox/*

%changelog
* Thu Oct 22 2026 buwryme <buwryme@proton.me> - 0.2.0-1
- Initial package
