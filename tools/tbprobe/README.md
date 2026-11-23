# tbprobe - the diagnostic harness for the Medula integration

A tiny .NET Framework 4.8 WinForms app that does the same things the vendor app does,
so we can see what Wine actually delivers instead of guessing:

* logs `Navigated` / `DocumentCompleted` / `ProgressChanged`, plus `Document.Url`,
  `DocumentText`, `Document.Forms` and each form's `action` attribute;
* replays the vendor's `GetStatus()` sequence step by step, logging the result (or the
  exception) of every call - the vendor wraps all of it in `try { } catch { }`, so its
  failures are silent;
* finds the SGK captcha `<img>`, casts it to `IHTMLElementRender` and calls `DrawToDC`
  into a bitmap, saving the PNG and reporting pixel statistics.

## Build (on Linux, no Windows and no root)

    curl -LO https://builds.dotnet.microsoft.com/dotnet/Sdk/8.0.425/dotnet-sdk-8.0.425-linux-x64.tar.gz
    tar xf dotnet-sdk-8.0.425-linux-x64.tar.gz
    export PATH=$PWD/dotnet:$PATH
    dotnet build -c Release          # net48 + Microsoft.NETFramework.ReferenceAssemblies

## Run under Wine

    cp bin/Release/net48/tbprobe.exe* ~/.wine-tebeos/drive_c/
    WINEPREFIX=~/.wine-tebeos wine 'C:\tbprobe.exe' 'https://medeczane.sgk.gov.tr/eczane/faces/login.jsp'
    cat ~/.wine-tebeos/drive_c/tbprobe.log

## What it established (2026-10-01)

* `DocumentCompleted` fires under Wine with `readyState=Complete`.
* On the live SGK page: `Document.Url` is `.../eczane/PostLogoutRedirectServlet` (what the
  vendor's `GetStatus()` compares against), `DocumentText` contains "Sosyal", `Document.Body`
  is present, all four login fields exist, and the captcha is `images[15]`
  (`/eczane/SayiUretenImageYeniServlet`, 140x30).
* The whole `GetStatus()` sequence succeeds - so the state machine should return 2 (login).
* `getAttribute("action")` used to return a resolved URL; after the Wine patch it returns the
  raw attribute (this is what made the vendor's `switch (Url)` match nothing).
* `(IHTMLElementRender)img` cast succeeds and `DrawToDC` is called (currently a stub, so the
  saved bitmap is blank - that is the remaining piece).
* The vendor app's own trace shows **no** `get_forms` / `getAttribute` calls, while this probe
  shows 2 / 40 with the same filter - i.e. the vendor app's `DocumentCompleted` handler never
  runs, even though the event itself fires.
