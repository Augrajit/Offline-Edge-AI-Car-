from huggingface_hub import hf_hub_download
import shutil, os
os.makedirs('model', exist_ok=True)
shutil.copy(hf_hub_download('Cactus-Compute/needle2','needle2.cact'), 'model/needle2.cact')
