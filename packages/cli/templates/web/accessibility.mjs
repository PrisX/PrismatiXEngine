export class WebAccessibility {
  constructor(root, locale) { this.root=root;this.locale=locale;this.elements=new Map();this.actions=[]; }
  enqueue(id,action,value="") { if(this.actions.length<128)this.actions.push({id,action,value:String(value)}); }
  takeActions() { return this.actions.splice(0); }
  speak(text,interrupt) {
    if(!globalThis.speechSynthesis)return;
    if(interrupt)speechSynthesis.cancel();
    const utterance=new SpeechSynthesisUtterance(text);utterance.lang=this.locale;
    const language=this.locale.toLowerCase(),voices=speechSynthesis.getVoices();
    utterance.voice=voices.find(voice=>voice.lang.toLowerCase()===language)
      ??voices.find(voice=>voice.lang.toLowerCase().split('-')[0]===language.split('-')[0])??null;
    speechSynthesis.speak(utterance);
  }
  publish(tree) {
    const present=new Set();
    const update=(node,parent)=>{
      present.add(node.id);
      let element=this.elements.get(node.id);
      if(!element){
        const tag=node.role==="button"?"button":node.role==="textbox"?(node.text?.multiline?"textarea":"input"):"div";
        element=document.createElement(tag);element.dataset.semanticId=node.id;this.elements.set(node.id,element);
        element.addEventListener('focus',()=>this.enqueue(node.id,'focus'));
        element.addEventListener('click',()=>{if(element.dataset.activate==='true')this.enqueue(node.id,'activate');});
        element.addEventListener('keydown',event=>{
          if(element.tagName!=='BUTTON'&&element.dataset.activate==='true'&&(event.key==='Enter'||event.key===' ')){
            event.preventDefault();this.enqueue(node.id,'activate');
          }
          if(element.getAttribute('role')==='slider'&&['ArrowRight','ArrowUp','ArrowLeft','ArrowDown'].includes(event.key)){
            event.preventDefault();this.enqueue(node.id,['ArrowRight','ArrowUp'].includes(event.key)?'increment':'decrement');
          }
          if(element.tagName==='INPUT'&&event.key==='Enter')this.enqueue(node.id,'submit');
        });
        element.addEventListener('input',()=>this.enqueue(node.id,'setValue',element.value));
        element.addEventListener('select',()=>{
          const bytes=index=>new TextEncoder().encode(element.value.slice(0,index)).length;
          this.enqueue(node.id,'setSelection',`${bytes(element.selectionStart)}:${bytes(element.selectionEnd)}`);
        });
      }
      if(element.parentNode!==parent)parent.append(element);
      element.hidden=node.hidden;
      element.setAttribute('role',node.role==='window'?'region':node.role==='text'?'note':node.role||'generic');
      element.setAttribute('aria-label',node.label||node.text?.content||'');
      if(node.description)element.setAttribute('aria-description',node.description);else element.removeAttribute('aria-description');
      element.tabIndex=node.focusable&&!node.hidden?0:-1;
      element.dataset.activate=String(node.actions.includes('activate'));
      element.dataset.text=node.text?.content??'';
      const disabled=node.states.includes('disabled');element.setAttribute('aria-disabled',String(disabled));
      if('disabled' in element)element.disabled=disabled;
      for(const state of ['checked','selected','expanded']){
        const negative={checked:'unchecked',selected:'unselected',expanded:'collapsed'}[state];
        if(node.states.includes(state)||node.states.includes(negative))element.setAttribute(`aria-${state}`,String(node.states.includes(state)));
        else element.removeAttribute(`aria-${state}`);
      }
      if(node.range){element.setAttribute('aria-valuemin',node.range.minimum);element.setAttribute('aria-valuemax',node.range.maximum);element.setAttribute('aria-valuenow',node.value);}
      if(node.text?.editable){
        element.readOnly=node.readOnly;
        if(element.value!==node.text.content)element.value=node.text.content;
      }else if(!node.children.length){
        const content=node.text?.content??(node.value||node.label||'');
        if(element.textContent!==content)element.textContent=content;
      }
      if(node.states.includes('focused')&&document.activeElement!==element&&this.root.contains(document.activeElement))element.focus({preventScroll:true});
      for(const child of node.children)update(child,element);
    };
    update(tree.root,this.root);
    for(const [id,element] of this.elements)if(!present.has(id)){element.remove();this.elements.delete(id);}
  }
  clear(){this.root.replaceChildren();this.elements.clear();this.actions=[];globalThis.speechSynthesis?.cancel();}
}
