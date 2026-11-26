# Draw overlays on a retained DirectComposition surface per monitor

The drag preview and toasts draw into one retained Direct2D surface per monitor, composed by DirectComposition on the overlay thread (`src/multiwm/app/overlay.h`), following PowerToys FancyZones. Rendering delay during a drag is visible to the user, and this approach cut it meaningfully.

## Considered Options

- **GDI layered windows**: measured far slower for these overlays. The preview went from DirectComposition (8c0e241) to GDI (5539789) to the retained surface (415a6f7); that history is settled, not churn to tidy up.
