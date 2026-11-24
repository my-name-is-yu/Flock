export function placeRows(container, rows) {
  const wanted = new Set(rows);
  for (const child of [...container.children]) if (!wanted.has(child)) child.remove();
  let next = container.firstElementChild;
  for (const row of rows) {
    if (row === next) next = next.nextElementSibling;
    else container.insertBefore(row, next);
  }
}

export function createTimelineRows(container, scrollRegion) {
  let scope;
  let current = new Map();
  return (nextScope, draw) => {
    const sameScope = nextScope === scope;
    const focused = sameScope && container.contains(document.activeElement) ? document.activeElement : null;
    const focusedRow = focused?.closest('[data-timeline-key]');
    const action = focused?.dataset.timelineAction;
    const next = new Map();
    const added = [];
    const rows = draw((key, inputs, render) => {
      const version = JSON.stringify(inputs);
      const previous = sameScope ? current.get(key) : null;
      const entry = previous?.version === version ? previous : { version, row: render() };
      if (entry !== previous) {
        entry.row.dataset.timelineKey = key;
        added.push(entry.row);
      }
      next.set(key, entry);
      return entry.row;
    });
    placeRows(container, rows);
    scope = nextScope;
    current = next;

    if (focusedRow && document.activeElement !== focused) {
      const row = next.get(focusedRow.dataset.timelineKey)?.row;
      const control = action && [...(row?.querySelectorAll('[data-timeline-action]') ?? [])]
        .find((element) => element.dataset.timelineAction === action && !element.matches(':disabled'));
      const target = control || row || scrollRegion;
      target.focus({ preventScroll: true });
      if (document.activeElement !== target) scrollRegion.focus({ preventScroll: true });
    }
    return added;
  };
}
