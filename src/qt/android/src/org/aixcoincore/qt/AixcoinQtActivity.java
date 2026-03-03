package org.aixcoincore.qt;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;

import org.qtproject.qt5.android.bindings.QtActivity;

import java.io.File;

public class AixcoinQtActivity extends QtActivity
{
    @Override
    public void onCreate(Bundle savedInstanceState)
    {
        final File aixcoinDir = new File(getFilesDir().getAbsolutePath() + "/.aixcoin");
        if (!aixcoinDir.exists()) {
            aixcoinDir.mkdir();
        }

        super.onCreate(savedInstanceState);
    }
}
