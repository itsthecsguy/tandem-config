# Tandem config for the Kinesis Advantage 360 Pro

A keyboard config for the Advantage 360 Pro: Kinesis's [Adv360-Pro-ZMK](https://github.com/KinesisCorporation/Adv360-Pro-ZMK) at V3.0, with its default layout and [Tandem](https://github.com/itsthecsguy/tandem) already set up. Tandem edits `config/adv360.keymap`, builds the firmware in this repo's GitHub Actions, and puts it on both halves.

## Use it

- On [tandemkb.com](https://tandemkb.com), sign in with GitHub and choose **Start a new repo**. Tandem makes your copy of this template.
- Or click **Use this template** here, then open tandemkb.com and choose your copy as a repo you have.

Either way you get a repo of your own with fresh history. It can be private, which a fork of a public repo can't.

## What's here

- `config/` Kinesis's config. `config/adv360.keymap` is your layout.
- `tandem/zmk-tandem/` Tandem's firmware module. It lets each half restart into update mode on request and report its build and layout.
- `.github/workflows/build.yml` Kinesis's build workflow, with the module added. Every push builds firmware for both halves.
- `bin/get_version.sh` and `settings-reset.uf2` from Kinesis. The workflow uses the script. Flashing the reset file to a half clears its saved settings, including Bluetooth pairings.

This template is published from the `template/` folder of [itsthecsguy/tandem](https://github.com/itsthecsguy/tandem). Send changes there.

## License

MIT. Kinesis's config files keep their copyright, and files from ZMK keep the license in their headers.
