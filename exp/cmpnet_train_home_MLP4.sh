cd ..
# End-2-End learning (randomly shuffle path)
python cmpnet_train.py --model_path /media/arclabdl1/HD1/YLmiao/results/CMPnet_res/home_mlp4/ \
--no_env 1 --no_motion_paths 1800 --grad_step 1 --learning_rate 0.001 \
--num_epochs 20 --memory_strength 0.5 --n_memories 5000 \
--n_tasks 1 --device 3 --freq_rehersal 100 --batch_rehersal 100 \
--start_epoch 0 --data_path /media/arclabdl1/HD1/YLmiao/data/home/ --world_size 20 --env_type home_mlp4 \
--memory_type res --total_input_size 150008 --AE_input_size 1 32 32 32 --mlp_input_size 142 --output_size 7
cd exp
