# XAML and C++/CX contract

The current MVP UI is intentionally small. The safe rule is: change behavior in code-behind first;
do not edit XAML merely to clean it up.

- `MainPage.g.hpp` is currently treated as a build input, not a casually regeneratable output.
- Any XAML control referenced by C++ must have a matching connection field and `Connect()` case.
- Keep the `x:Name` values used by the code-behind unchanged.
- Runtime visibility changes are allowed when they avoid changing the generated glue contract.
- Verify with the repository's XAML/Connect checker after any XAML change.
- If a future XAML cleanup is attempted, first make a throwaway C++/CX UWP reproduction and prove that
  glue regeneration works on the current toolchain.

Current MVP controls: address bar, Back, content surface, and the minimal bottom chrome. History and
suggestions are implemented without introducing another persistent panel over the page.
