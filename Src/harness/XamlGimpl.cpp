// XamlGimpl.cpp — compiles XAML-generated .g.hpp implementation files.
// VS18 XAML compiler generates implementation in .g.hpp but the .g.cpp is empty.
#include "pch.h"
#include "App.g.hpp"
// Apotheosis: this translation unit is excluded from the build (Harness.vcxproj carries
// Condition="'false'" on it) because App::InitializeComponent is hand-written in App.xaml.cpp --
// App.xaml has no resources, so App.xbf is deliberately never loaded. MainPage.g.hpp is included
// at the bottom of MainPage.xaml.cpp instead, which keeps Connect() in step with the markup.
