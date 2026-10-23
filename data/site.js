const nav = document.querySelector("#site-nav");
const toggle = document.querySelector(".nav-toggle");
const stylesheet = document.querySelector('link[href$="theme.css"]');
const currentRoute = location.pathname === "/blinds" || location.pathname.endsWith("blinds.html")
  ? "blinds"
  : location.pathname === "/info" || location.pathname.endsWith("info.html")
    ? "system" : "devices";

for (const link of nav?.querySelectorAll("[data-route]") ?? []) {
  if (link.dataset.route === currentRoute) link.setAttribute("aria-current", "page");
}

if (nav && toggle) {
  const scrim = document.createElement("button");
  scrim.className = "nav-scrim";
  scrim.type = "button";
  scrim.tabIndex = -1;
  scrim.setAttribute("aria-label", "Chiudi navigazione");
  scrim.hidden = true;
  document.body.append(scrim);

  const setOpen = (open) => {
    document.body.classList.toggle("nav-open", open);
    toggle.setAttribute("aria-expanded", String(open));
    toggle.setAttribute("aria-label", open ? "Chiudi navigazione" : "Apri navigazione");
    scrim.hidden = !open;
  };

  toggle.addEventListener("click", () => setOpen(toggle.getAttribute("aria-expanded") !== "true"));
  scrim.addEventListener("click", () => setOpen(false));
  nav.addEventListener("click", (event) => {
    if (event.target.closest("a")) setOpen(false);
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") setOpen(false);
  });
}

const hasAppliedTheme = stylesheet && [...document.styleSheets].some((sheet) => {
  if (sheet.href !== stylesheet.href) return false;
  try {
    return sheet.cssRules.length > 0;
  } catch {
    return true;
  }
});

if (stylesheet && !hasAppliedTheme) {
  fetch(stylesheet.href)
    .then((response) => response.text())
    .then((css) => {
      const sharedTheme = document.createElement("style");
      sharedTheme.textContent = css;
      document.head.append(sharedTheme);
    });
}