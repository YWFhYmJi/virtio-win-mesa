/**************************************************************************
 *
 * Copyright 2026
 * SPDX-License-Identifier: MIT
 *
 **************************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <d3d10.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static void
Log(const char *format, ...)
{
   char buffer[1024];

   va_list ap;
   va_start(ap, format);
   vsnprintf(buffer, sizeof(buffer), format, ap);
   va_end(ap);

   fputs(buffer, stdout);
   OutputDebugStringA(buffer);
}

static bool
ParseUIntArg(int argc, char **argv, int *index, unsigned *value)
{
   if (*index + 1 >= argc)
      return false;

   char *end = nullptr;
   unsigned long parsed = strtoul(argv[*index + 1], &end, 0);
   if (!end || *end)
      return false;

   *value = (unsigned)parsed;
   (*index)++;
   return true;
}

static LRESULT CALLBACK
WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
   if (msg == WM_CLOSE || msg == WM_DESTROY) {
      PostQuitMessage(0);
      return 0;
   }

   return DefWindowProc(hwnd, msg, wparam, lparam);
}

int
main(int argc, char **argv)
{
   unsigned width = 800;
   unsigned height = 600;
   unsigned frames = 600;
   unsigned sync_interval = 0;
   unsigned sleep_ms = 16;
   bool fullscreen = false;
   bool debug = false;

   for (int i = 1; i < argc; ++i) {
      if (!strcmp(argv[i], "--fullscreen")) {
         fullscreen = true;
      } else if (!strcmp(argv[i], "--debug")) {
         debug = true;
      } else if (!strcmp(argv[i], "--frames")) {
         if (!ParseUIntArg(argc, argv, &i, &frames))
            return EXIT_FAILURE;
      } else if (!strcmp(argv[i], "--sync")) {
         if (!ParseUIntArg(argc, argv, &i, &sync_interval))
            return EXIT_FAILURE;
      } else if (!strcmp(argv[i], "--sleep")) {
         if (!ParseUIntArg(argc, argv, &i, &sleep_ms))
            return EXIT_FAILURE;
      } else if (!strcmp(argv[i], "--width")) {
         if (!ParseUIntArg(argc, argv, &i, &width))
            return EXIT_FAILURE;
      } else if (!strcmp(argv[i], "--height")) {
         if (!ParseUIntArg(argc, argv, &i, &height))
            return EXIT_FAILURE;
      } else {
         Log("discard_present: unknown argument '%s'\n", argv[i]);
         return EXIT_FAILURE;
      }
   }

   Log("discard_present: start pid=%lu size=%ux%u frames=%u sync=%u sleep_ms=%u fullscreen=%u debug=%u swap_effect=DISCARD\n",
       GetCurrentProcessId(), width, height, frames, sync_interval, sleep_ms,
       fullscreen, debug);

   HINSTANCE instance = GetModuleHandle(nullptr);

   WNDCLASSEXA wc = {};
   wc.cbSize = sizeof(wc);
   wc.style = CS_HREDRAW | CS_VREDRAW;
   wc.lpfnWndProc = WindowProc;
   wc.hInstance = instance;
   wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
   wc.lpszClassName = "discard_present";

   if (!RegisterClassExA(&wc)) {
      Log("discard_present: RegisterClassEx failed err=%lu\n", GetLastError());
      return EXIT_FAILURE;
   }

   DWORD style = WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_OVERLAPPEDWINDOW;
   RECT rect = {0, 0, (LONG)width, (LONG)height};
   AdjustWindowRect(&rect, style, FALSE);

   HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "D3D10 discard present",
                               style, CW_USEDEFAULT, CW_USEDEFAULT,
                               rect.right - rect.left, rect.bottom - rect.top,
                               nullptr, nullptr, instance, nullptr);
   if (!hwnd) {
      Log("discard_present: CreateWindowEx failed err=%lu\n", GetLastError());
      return EXIT_FAILURE;
   }

   ShowWindow(hwnd, SW_SHOWDEFAULT);
   UpdateWindow(hwnd);

   DXGI_SWAP_CHAIN_DESC swap_desc = {};
   swap_desc.BufferDesc.Width = width;
   swap_desc.BufferDesc.Height = height;
   swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   swap_desc.BufferDesc.RefreshRate.Numerator = 60;
   swap_desc.BufferDesc.RefreshRate.Denominator = 1;
   swap_desc.SampleDesc.Count = 1;
   swap_desc.SampleDesc.Quality = 0;
   swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   swap_desc.BufferCount = 2;
   swap_desc.OutputWindow = hwnd;
   swap_desc.Windowed = fullscreen ? FALSE : TRUE;
   swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
   swap_desc.Flags = fullscreen ? DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH : 0;

   UINT create_flags = debug ? D3D10_CREATE_DEVICE_DEBUG : 0;

   ComPtr<ID3D10Device> device;
   ComPtr<IDXGISwapChain> swap_chain;
   HRESULT hr = D3D10CreateDeviceAndSwapChain(nullptr,
                                              D3D10_DRIVER_TYPE_HARDWARE,
                                              nullptr,
                                              create_flags,
                                              D3D10_SDK_VERSION,
                                              &swap_desc,
                                              &swap_chain,
                                              &device);
   if (FAILED(hr)) {
      Log("discard_present: D3D10CreateDeviceAndSwapChain failed hr=0x%08lx\n",
          (unsigned long)hr);
      return EXIT_FAILURE;
   }

   ComPtr<IDXGIDevice> dxgi_device;
   hr = device.As(&dxgi_device);
   if (SUCCEEDED(hr)) {
      ComPtr<IDXGIAdapter> adapter;
      hr = dxgi_device->GetAdapter(&adapter);
      if (SUCCEEDED(hr)) {
         DXGI_ADAPTER_DESC desc = {};
         hr = adapter->GetDesc(&desc);
         if (SUCCEEDED(hr))
            Log("discard_present: adapter=%S\n", desc.Description);
      }
   }

   ComPtr<ID3D10Texture2D> back_buffer;
   hr = swap_chain->GetBuffer(0, __uuidof(ID3D10Texture2D),
                              (void **)back_buffer.GetAddressOf());
   if (FAILED(hr)) {
      Log("discard_present: GetBuffer failed hr=0x%08lx\n", (unsigned long)hr);
      return EXIT_FAILURE;
   }

   ComPtr<ID3D10RenderTargetView> rtv;
   hr = device->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv);
   if (FAILED(hr)) {
      Log("discard_present: CreateRenderTargetView failed hr=0x%08lx\n",
          (unsigned long)hr);
      return EXIT_FAILURE;
   }

   D3D10_VIEWPORT viewport = {};
   viewport.Width = width;
   viewport.Height = height;
   viewport.MinDepth = 0.0f;
   viewport.MaxDepth = 1.0f;
   device->RSSetViewports(1, &viewport);
   device->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

   bool quit = false;
   MSG msg;
   for (unsigned frame = 0; frame < frames && !quit; ++frame) {
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
         if (msg.message == WM_QUIT) {
            quit = true;
            break;
         }
         TranslateMessage(&msg);
         DispatchMessage(&msg);
      }
      if (quit)
         break;

      float t = (float)(frame % 240) / 239.0f;
      float clear[4] = {t, 0.25f, 1.0f - t, 1.0f};
      device->ClearRenderTargetView(rtv.Get(), clear);

      hr = swap_chain->Present(sync_interval, 0);
      if (frame < 32 || (frame & 255) == 0 || FAILED(hr)) {
         Log("discard_present: present frame=%u hr=0x%08lx sync=%u fullscreen=%u\n",
             frame, (unsigned long)hr, sync_interval, fullscreen);
      }
      if (FAILED(hr))
         break;

      if (sleep_ms)
         Sleep(sleep_ms);
   }

   if (fullscreen)
      swap_chain->SetFullscreenState(FALSE, nullptr);

   device->OMSetRenderTargets(0, nullptr, nullptr);
   rtv.Reset();
   back_buffer.Reset();
   swap_chain.Reset();
   device.Reset();

   DestroyWindow(hwnd);
   Log("discard_present: done\n");

   return FAILED(hr) ? EXIT_FAILURE : EXIT_SUCCESS;
}
