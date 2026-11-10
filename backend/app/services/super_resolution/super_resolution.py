#import os
import torch
import numpy as np
from .state import load_model_state_dict
from .cosmology import dis
from .displacement import pos2dis, dis2pos
#from pathlib import Path
#from astropy.io import fits

from .srsgan2 import *

def load_sr_model(weights_path: str, device: str = "cpu"):
    model = G(3,3,2) # NOTE: hardcoded for now
    state = torch.load(weights_path, map_location=device)
    load_model_state_dict(model, state['model'])
    model.to(device)
    model.eval()
    return model

@torch.inference_mode()
def run_sr_on_fits(arr: np.ndarray, model, scale: int, device: str):
    arr = np.asarray(arr, dtype=np.float32)
    x = torch.from_numpy(arr).unsqueeze(0).unsqueeze(0).to(device)  # [1,1,H,W]
    y = model(x)
    out = y.detach().cpu().numpy()[0, 0]
    return out.astype(np.float32)

@torch.inference_mode()
def run_sr_on_nbody(arr: np.ndarray, model, upsample_fac: int, device: str, kind: str = None, cosmology: str = None, particle_mass: float = None):
    x = torch.from_numpy(np.array(arr, dtype=np.float32, copy=True)).to(device)
    x = x.squeeze(0).permute(1, 2, 3, 0)
    x = pos2dis(x, 1000 * 16/1024, 16) #NOTE: hardcoded
    dis(x)
    x = x.permute(3, 0, 1, 2).unsqueeze(0)
    y = model(x)
    y = y.squeeze(0)
    dis(y, undo=True)
    y = y.detach().cpu().numpy()
    y = dis2pos(y, 1000 * 16/1024, 26) #NOTE: hardcoded!
    return np.expand_dims(y, axis=0)