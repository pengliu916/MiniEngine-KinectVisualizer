#include "pch.h"
#include "KinectVisualizer.h"

_Use_decl_annotations_
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    KinectVisualizer application(1440, 1024);
    return Core::Run(application, hInstance, nCmdShow);
}
