'use strict';
// Controller names are descriptive only. Every connected Gamepad is eligible.
const controllerInput = (() => {
  const labels = [
    'A',
    'B',
    'X',
    'Y',
    'Left stick click',
    'Right stick click',
    'L',
    'R',
    'ZL',
    'ZR',
    '+',
    '−',
    'D-pad left',
    'D-pad up',
    'D-pad right',
    'D-pad down',
    'HOME',
  ];
  const defaults = [1, 0, 3, 2, 10, 11, 4, 5, 6, 7, 9, 8, 14, 12, 15, 13, 16];
  const layouts = new Map();
  let current = null,
    chosen = null,
    signature = '',
    editing = false,
    changed = () => {};
  const element = (id) => document.getElementById(id),
    key = (id) => 'nxrp.controller.v1:' + id;
  function defaultLayout() {
    return {
      buttons: defaults.map((index) => ({ kind: 'button', index })),
      axes: [0, 1, 2, 3].map((index) => ({ index, invert: false })),
    };
  }
  function valid(layout) {
    const index = (value) => Number.isInteger(value) && value >= 0 && value < 256;
    return (
      layout &&
      Array.isArray(layout.buttons) &&
      layout.buttons.length === 17 &&
      layout.buttons.every(
        (binding) =>
          binding === null ||
          (binding &&
            index(binding.index) &&
            (binding.kind === 'button' ||
              (binding.kind === 'axis' && (binding.direction === 1 || binding.direction === -1)))),
      ) &&
      Array.isArray(layout.axes) &&
      layout.axes.length === 4 &&
      layout.axes.every(
        (axis) => axis === null || (axis && index(axis.index) && typeof axis.invert === 'boolean'),
      )
    );
  }
  function storage() {
    try {
      return window.localStorage;
    } catch {
      return null;
    }
  }
  function layoutFor(pad) {
    if (!layouts.has(pad.id)) {
      let saved = null;
      try {
        saved = JSON.parse(storage()?.getItem(key(pad.id)) || 'null');
      } catch {}
      layouts.set(pad.id, valid(saved) ? saved : null);
    }
    return layouts.get(pad.id) || defaultLayout();
  }
  function setLayout(pad, layout) {
    if (!valid(layout)) throw new Error('Invalid controller layout');
    layouts.set(pad.id, layout);
    try {
      const store = storage();
      if (!store) return false;
      store.setItem(key(pad.id), JSON.stringify(layout));
      return true;
    } catch {
      return false;
    }
  }
  function controllerList(pads) {
    const next = pads.map((pad) => `${pad.index}:${pad.id}`).join('|');
    if (next === signature) return;
    signature = next;
    const select = element('controller-select');
    select.hidden = pads.length < 2;
    if (typeof document.createElement !== 'function') return;
    select.replaceChildren();
    for (const pad of pads) {
      const option = document.createElement('option');
      option.value = String(pad.index);
      option.textContent = pad.id;
      select.append(option);
    }
    if (current) select.value = String(current.index);
  }
  // Button glyphs follow the controller that is connected, like the Android app: Xbox / generic = coloured A B X Y,
  // PlayStation = cross, circle, square, triangle, Nintendo = B A Y X. Positions follow the standard layout:
  // 0 = bottom, 1 = right, 2 = left, 3 = top.
  function padStyle(pad) {
    const id = ((pad && pad.id) || '').toLowerCase();
    if (/054c|dualsense|dualshock|playstation|wireless controller/.test(id)) return 'ps';
    if (/057e|nintendo|pro controller|joy-con/.test(id)) return 'nintendo';
    return 'xbox';
  }
  function glyphSvg(style, position) {
    const open = '<svg viewBox="0 0 24 24" aria-hidden="true">';
    if (style !== 'ps') {
      const letter = (style === 'nintendo' ? ['B', 'A', 'Y', 'X'] : ['A', 'B', 'X', 'Y'])[position];
      const fill =
        style === 'nintendo' ? '#454b5a' : ['#43a047', '#e53935', '#1e88e5', '#fdd835'][position];
      const ink = style === 'xbox' && position === 3 ? '#332b00' : '#fff';
      return (
        open +
        '<circle cx="12" cy="12" r="12" fill="' +
        fill +
        '"/><text x="12" y="17" text-anchor="middle" font-family="Arial,Helvetica,sans-serif" font-weight="700" font-size="15" fill="' +
        ink +
        '">' +
        letter +
        '</text></svg>'
      );
    }
    const stroke = 'fill="none" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"';
    const shape = [
      '<path d="M7.6 7.6 16.4 16.4M16.4 7.6 7.6 16.4" ' + stroke + ' stroke="#7cb4ff"/>',
      '<circle cx="12" cy="12" r="5.6" ' + stroke + ' stroke="#ff6b7a"/>',
      '<rect x="7.4" y="7.4" width="9.2" height="9.2" ' + stroke + ' stroke="#e48ae4"/>',
      '<path d="M12 6.6 17.2 15.8H6.8Z" ' + stroke + ' stroke="#4dd6a4"/>',
    ][position];
    return open + '<circle cx="12" cy="12" r="12" fill="#2a3042"/>' + shape + '</svg>';
  }
  let legendKey = '';
  function renderLegend(pad) {
    const legend = element('padLegend');
    if (!legend) return;
    if (!pad) {
      legend.hidden = true;
      legendKey = '';
      return;
    }
    const layout = layoutFor(pad),
      style = padStyle(pad);
    const entries = [0, 1, 2, 3].map((bit) => {
      const binding = layout.buttons[bit];
      return binding && binding.kind === 'button' && binding.index < 4 && pad.mapping === 'standard'
        ? { bit, position: binding.index }
        : null;
    });
    const key = pad.id + '|' + style + '|' + JSON.stringify(entries);
    if (key === legendKey) return;
    legendKey = key;
    if (entries.some((entry) => !entry)) {
      legend.hidden = true;
      return;
    }
    legend.innerHTML = entries
      .map(
        (entry) =>
          '<span class="padchip">' +
          glyphSvg(style, entry.position) +
          '<span class="to">&rarr; ' +
          labels[entry.bit] +
          '</span></span>',
      )
      .join('');
    legend.hidden = false;
  }
  function poll() {
    const indicator = element('controller');
    if (window.isSecureContext === false) {
      indicator.textContent =
        'Controller access requires a secure browser connection. Open the HTTPS browser link. On-screen controls are available.';
      renderLegend(null);
      return null;
    }
    if (typeof navigator.getGamepads !== 'function') {
      indicator.textContent =
        'This browser does not expose controller input. On-screen controls are available.';
      renderLegend(null);
      return null;
    }
    let pads;
    try {
      pads = Array.from(navigator.getGamepads() || []).filter((pad) => pad?.connected);
    } catch {
      indicator.textContent =
        'Browser blocked controller access. On-screen controls are available.';
      renderLegend(null);
      return null;
    }
    const previous = current;
    current =
      pads.find((pad) => pad.index === chosen) ||
      pads.find((pad) => pad.index === current?.index) ||
      pads[0] ||
      null;
    controllerList(pads);
    element('controller-configure').disabled = !current;
    if (previous && current?.id !== previous.id) {
      editing = false;
      element('controller-mapping').hidden = true;
    }
    if (!current) {
      indicator.textContent = 'Press a controller button to detect your gamepad.';
      renderLegend(null);
      return null;
    }
    layoutFor(current);
    renderLegend(current);
    if (editing) {
      const buttons = current.buttons
        .map((button, index) => (buttonValue(button) > 0.5 ? index : null))
        .filter((index) => index !== null);
      element('controller-live').textContent =
        'Pressed buttons: ' +
        (buttons.join(', ') || 'none') +
        ' • Axes: ' +
        current.axes.map((value, index) => index + ': ' + Number(value).toFixed(2)).join(', ');
    }
    indicator.textContent =
      'Controller: ' +
      current.id +
      (editing
        ? ' • Mapping controls'
        : layouts.get(current.id)
          ? ' • Saved layout'
          : current.mapping === 'standard'
            ? ' • Standard layout'
            : ' • Default layout; configure buttons if needed');
    return editing ? null : current;
  }
  function buttonValue(button) {
    return typeof button === 'number' ? button : button?.pressed ? 1 : Number(button?.value) || 0;
  }
  function apply(pad, state) {
    const layout = layoutFor(pad);
    layout.buttons.forEach((binding, bit) => {
      if (!binding) return;
      const down =
        binding.kind === 'button'
          ? buttonValue(pad.buttons[binding.index]) > 0.5
          : (Number(pad.axes[binding.index]) || 0) * binding.direction > 0.5;
      if (down) {
        if (bit === 16) state.home = true;
        else state.keys |= 1 << bit;
      }
    });
    layout.axes.forEach((axis, index) => {
      if (!axis) return;
      const raw = Number(pad.axes[axis.index]);
      if (!Number.isFinite(raw)) return;
      const value = Math.max(-1, Math.min(1, raw)) * (axis.invert ? -1 : 1);
      state.axes[index] =
        Math.abs(value) < 0.15 ? 0 : Math.round(value * (index % 2 ? -32767 : 32767));
    });
  }
  function options(select, axisOnly = false) {
    const add = (value, text) => {
      const option = document.createElement('option');
      option.value = value;
      option.textContent = text;
      select.append(option);
    };
    add('', 'Unassigned');
    if (!axisOnly)
      for (let index = 0; index < current.buttons.length; index++)
        add('b:' + index, 'Button ' + index);
    for (let index = 0; index < current.axes.length; index++) {
      if (axisOnly) add('a:' + index, 'Axis ' + index);
      else {
        add('p:' + index, 'Axis ' + index + ' positive');
        add('n:' + index, 'Axis ' + index + ' negative');
      }
    }
  }
  function renderMapping() {
    if (!current) return;
    const rows = element('controller-bindings');
    rows.replaceChildren();
    const layout = layoutFor(current);
    const row = (labelText, id, axisOnly) => {
      const label = document.createElement('label');
      label.textContent = labelText;
      const select = document.createElement('select');
      select.id = id;
      options(select, axisOnly);
      label.append(select);
      rows.append(label);
      return { label, select };
    };
    labels.forEach((name, index) => {
      const { select } = row(name, 'controller-button-' + index, false),
        binding = layout.buttons[index];
      select.value = binding
        ? (binding.kind === 'button' ? 'b:' : binding.direction === 1 ? 'p:' : 'n:') + binding.index
        : '';
    });
    [
      'Left stick horizontal',
      'Left stick vertical',
      'Right stick horizontal',
      'Right stick vertical',
    ].forEach((name, index) => {
      const { label, select } = row(name, 'controller-axis-' + index, true),
        axis = layout.axes[index];
      select.value = axis ? 'a:' + axis.index : '';
      const invertLabel = document.createElement('span'),
        invert = document.createElement('input');
      invert.type = 'checkbox';
      invert.id = 'controller-invert-' + index;
      invert.checked = axis?.invert || false;
      invertLabel.append(invert, document.createTextNode(' Reverse direction'));
      label.append(invertLabel);
    });
    element('controller-save-status').textContent = '';
  }
  function init(onChange) {
    changed = onChange;
    element('controller-select').addEventListener('change', (event) => {
      chosen = Number(event.target.value);
      editing = false;
      element('controller-mapping').hidden = true;
      current = null;
      changed();
    });
    element('controller-configure').addEventListener('click', () => {
      if (!current) return;
      editing = !editing;
      element('controller-mapping').hidden = !editing;
      if (editing) renderMapping();
      changed();
    });
    element('controller-save').addEventListener('click', () => {
      if (!current) return;
      const layout = { buttons: [], axes: [] };
      for (let index = 0; index < 17; index++) {
        const value = element('controller-button-' + index).value,
          [kind, number] = value.split(':');
        layout.buttons.push(
          value
            ? {
                kind: kind === 'b' ? 'button' : 'axis',
                index: Number(number),
                ...(kind === 'b' ? {} : { direction: kind === 'p' ? 1 : -1 }),
              }
            : null,
        );
      }
      for (let index = 0; index < 4; index++) {
        const value = element('controller-axis-' + index).value;
        layout.axes.push(
          value
            ? {
                index: Number(value.slice(2)),
                invert: element('controller-invert-' + index).checked,
              }
            : null,
        );
      }
      const saved = setLayout(current, layout);
      editing = false;
      element('controller-mapping').hidden = true;
      element('controller-save-status').textContent = saved
        ? 'Controller layout saved.'
        : 'Layout applied for this visit; browser storage is unavailable.';
      changed();
    });
    element('controller-reset').addEventListener('click', () => {
      if (!current) return;
      layouts.set(current.id, null);
      try {
        storage()?.removeItem(key(current.id));
      } catch {}
      renderMapping();
      changed();
    });
    element('controller-cancel').addEventListener('click', () => {
      editing = false;
      element('controller-mapping').hidden = true;
      changed();
    });
  }
  return { init, poll, apply, layoutFor, setLayout, valid, defaultLayout };
})();
