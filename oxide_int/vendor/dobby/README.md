# vendor/dobby

`arm64-v8a/libdobby.a` + `dobby.h` взяты как есть из архива `@dearvision.zip`
(`NonRootLoader/NonRootLoader/src/main/jni/{include/Hook/Dobby/dobby.h,lib/arm64-v8a/libdobby.a}`,
origin/main). Это статическая сборка Dobby (inline hook, BSD-3-Clause, github.com/jmpews/Dobby)
от 2023-11-06; собиралась под NDK r22 / `APP_STL := c++_static`, поэтому линкуется только
вместе с `-static-libstdc++` (в архиве есть ссылки на `_Znwm`, `__cxa_*`, `std::logic_error`).

Что используем:
* `DobbyHook` на `sigaction` — не даём никому (Crashlytics) снять наш fault recorder;
* `DobbyHook` на `dlopen` / `android_dlopen_ext` — эксперимент «force GLES» (не пускать
  `libvulkan.so`, как это делает deavision-загрузчик) + лог всех чужих библиотек.

Чего не тащим: ImGui (~47k строк) и весь render-стек deavision. У нас оверлей — обычный
`View` в своём окне, GL-хук на `eglSwapBuffers` избыточен и заметнее.
