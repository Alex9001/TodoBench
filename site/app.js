// Progressive enhancement: downloads, guides, and screenshot links work without JS.
const themeImage = document.querySelector('#theme-image');
const themeLink = document.querySelector('#theme-link');
const themeCaption = document.querySelector('#theme-caption');
const themeButtons = document.querySelectorAll('[data-theme]');
for (const button of themeButtons) {
  button.addEventListener('click', () => {
    const theme = button.dataset.theme;
    const label = button.textContent.trim();
    themeImage.src = `assets/screenshots/${theme}.png`;
    themeImage.alt = `TodoBench in the ${label} theme, showing task recurrence and a Markdown checklist`;
    themeLink.href = themeImage.src;
    themeCaption.textContent = `${label} theme · Choose View → Appearance in TodoBench.`;
    for (const choice of themeButtons) choice.setAttribute('aria-pressed', String(choice === button));
  });
}
const dialog = document.querySelector('#screenshot-dialog');
const previewImage = document.querySelector('#preview-image');
if (typeof dialog.showModal === 'function') {
  for (const link of document.querySelectorAll('[data-preview]')) {
    link.addEventListener('click', event => {
      if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey || event.button !== 0) return;
      event.preventDefault();
      previewImage.src = link.href;
      previewImage.alt = link.querySelector('img').alt;
      document.querySelector('#original-link').href = link.href;
      dialog.showModal();
      document.querySelector('#close-preview').focus();
    });
  }
  document.querySelector('#close-preview').addEventListener('click', () => dialog.close());
  dialog.addEventListener('click', event => {
    const bounds = dialog.getBoundingClientRect();
    if (event.target === dialog && (event.clientX < bounds.left || event.clientX > bounds.right || event.clientY < bounds.top || event.clientY > bounds.bottom)) dialog.close();
  });
}
