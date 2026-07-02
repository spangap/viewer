# Viewer

A tiny **HTML** / *Markdown* document viewer.

- Renders local Markdown and HTML, plus images and links served by this device
- Markdown is converted to HTML by the device's own web server
- Use the **Back** button to return to the previous page

Try a link: [About this viewer](ABOUT.md)

See the [build manifest](BUILD.md) for the exact commit of every straddle in
this image.

Open a document from the CLI:

```
webview /sdcard/readme.md     # in the web viewer window
lcdview /sdcard/readme.md     # on the LCD
```

---

Emphasis is shown by colour: **bold**, *italic*, and `code`.
