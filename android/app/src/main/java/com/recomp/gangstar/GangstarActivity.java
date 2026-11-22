package com.recomp.gangstar;

import org.libsdl.app.SDLActivity;

/** SDL's activity running libmain.so (the native port). */
public class GangstarActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "main"};
    }
}
